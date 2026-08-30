#include <npk/arch.h>
#include <npk/console.h>
#include <npk/gop.h>
#include <npk/elf.h>
#include <npk/heap.h>
#include <npk/log.h>
#include <npk/memory.h>
#include <npk/process.h>
#include <npk/string.h>
#include <npk/syscall.h>
#include <npk/timer.h>
#include <npk/vfs.h>
#include <npk/vm.h>
#include <npk/keyboard.h>

extern void syscall_entry(void);
static process_t *current_process;

#define EFAULT 14
#define EBADF 9
#define EINVAL 22
#define ENOMEM 12
#define ENODEV 19
#define ENOSYS 38
#define ENAMETOOLONG 36
#define ENOENT 2
#define ECHILD 10
#define ESRCH 3
#define EAGAIN 11
#define EMFILE 24
#define EINTR 4
#define EPERM 1
#define ENOTTY 25
#define EOVERFLOW 75
#define WNOHANG 1ULL
#define IOV_MAX_NPK 1024U
#define ARCH_SET_GS 0x1001U
#define ARCH_SET_FS 0x1002U
#define ARCH_GET_FS 0x1003U
#define ARCH_GET_GS 0x1004U
#define PATH_MAX_NPK 256U
#define IO_CHUNK 4096U
#define EXEC_MAX_IMAGE (16U * 1024U * 1024U)

typedef struct { int64_t seconds; int64_t nanoseconds; } npk_timespec_t;
typedef struct { char sysname[65], nodename[65], release[65], version[65], machine[65], domainname[65]; } npk_utsname_t;
typedef struct {
    uint32_t c_iflag, c_oflag, c_cflag, c_lflag;
    uint8_t c_line;
    uint8_t c_cc[32];
    uint32_t c_ispeed, c_ospeed;
} npk_termios_t;
typedef struct {
    uint16_t rows, columns, x_pixels, y_pixels;
} npk_winsize_t;

#define NPK_IOCTL_TCGETS 0x5401ULL
#define NPK_IOCTL_TCSETS 0x5402ULL
#define NPK_IOCTL_TCSETSW 0x5403ULL
#define NPK_IOCTL_TCSETSF 0x5404ULL
#define NPK_IOCTL_TIOCGWINSZ 0x5413ULL
#define NPK_IOCTL_TIOCSWINSZ 0x5414ULL
#define NPK_IOCTL_FIONREAD 0x541bULL

static npk_termios_t terminal_settings = {
    .c_iflag = 0x00000100U,
    .c_oflag = 0x00000001U,
    .c_cflag = 0x000000bfU,
    .c_lflag = 0x0000000aU,
    .c_cc = { [5] = 0, [6] = 1 },
    .c_ispeed = 15U,
    .c_ospeed = 15U,
};
static npk_winsize_t terminal_winsize = { .rows = 25, .columns = 80, .x_pixels = 0, .y_pixels = 0 };

static uint64_t neg_errno(uint64_t error) { return (uint64_t)(-(int64_t)error); }
static int process_handle_for_fd(const process_t *process, int fd);

static bool copy_user_string(vaddr_t user, char *kernel, size_t capacity) {
    if (kernel == NULL || capacity == 0 || !vmm_is_user_range(user, 1)) return false;
    for (size_t i = 0; i + 1 < capacity; ++i) {
        if (!vmm_copyin(&kernel[i], user + i, 1)) return false;
        if (kernel[i] == '\0') return true;
    }
    kernel[capacity - 1] = '\0';
    return false;
}

static uint64_t sys_write(uint64_t fd, vaddr_t user_buffer, uint64_t count) {
    process_t *process = process_current();
    if (!process || fd >= NPK_MAX_FDS || process_handle_for_fd(process, (int)fd) < 0)
        return neg_errno(EBADF);
    if (!vmm_is_user_range(user_buffer, count)) return neg_errno(EFAULT);
    char chunk[IO_CHUNK];
    uint64_t written = 0;
    int handle = process_handle_for_fd(process, (int)fd);
    while (written < count) {
        uint64_t amount = count - written < sizeof(chunk) ? count - written : sizeof(chunk);
        if (!vmm_copyin(chunk, user_buffer + written, (size_t)amount)) return neg_errno(EFAULT);
        if (handle == 1 || handle == 2) {
            console_write_n(chunk, (size_t)amount);
            written += amount;
            continue;
        }
        ssize_t result = vfs_write(handle, chunk, (size_t)amount);
        if (result < 0) return written ? written : (uint64_t)result;
        written += (uint64_t)result;
        if ((uint64_t)result < amount) break;
    }
    return written;
}

typedef struct {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t st_pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_atime;
    int64_t st_atime_nsec;
    int64_t st_mtime;
    int64_t st_mtime_nsec;
    int64_t st_ctime;
    int64_t st_ctime_nsec;
    int64_t st_reserved[3];
} npk_linux_stat_t;

static uint64_t sys_fstat(uint64_t fd, vaddr_t user_status) {
    process_t *process = process_current();
    int handle = process && fd < NPK_MAX_FDS ? process_handle_for_fd(process, (int)fd) : -1;
    if (handle < 0) return neg_errno(EBADF);
    npk_linux_stat_t output;
    vfs_stat_t input;
    int result = vfs_stat_fd(handle, &input);
    if (result < 0) return (uint64_t)result;
    memset(&output, 0, sizeof(output));
    output.st_dev = 1;
    output.st_ino = input.inode;
    output.st_nlink = 1;
    output.st_mode = input.mode;
    output.st_uid = input.uid;
    output.st_gid = input.gid;
    output.st_size = (int64_t)input.size;
    output.st_blksize = 512;
    output.st_blocks = (int64_t)input.blocks;
    return vmm_copyout(user_status, &output, sizeof(output)) ? 0 : neg_errno(EFAULT);
}

static uint64_t sys_stat_path(vaddr_t user_path, vaddr_t user_status) {
    char path[PATH_MAX_NPK];
    if (!copy_user_string(user_path, path, sizeof(path))) return neg_errno(EFAULT);
    int handle = vfs_open(path);
    if (handle < 0) return (uint64_t)handle;
    npk_linux_stat_t output;
    vfs_stat_t input;
    int result = vfs_stat_fd(handle, &input);
    vfs_close(handle);
    if (result < 0) return (uint64_t)result;
    memset(&output, 0, sizeof(output));
    output.st_dev = 1; output.st_ino = input.inode; output.st_nlink = 1;
    output.st_mode = input.mode; output.st_uid = input.uid; output.st_gid = input.gid;
    output.st_size = (int64_t)input.size; output.st_blksize = 512; output.st_blocks = (int64_t)input.blocks;
    return vmm_copyout(user_status, &output, sizeof(output)) ? 0 : neg_errno(EFAULT);
}

static uint64_t sys_lseek(uint64_t fd, uint64_t raw_offset, uint64_t whence) {
    process_t *process = process_current();
    int handle = process && fd < NPK_MAX_FDS ? process_handle_for_fd(process, (int)fd) : -1;
    if (handle < 0) return neg_errno(EBADF);
    int64_t offset = (int64_t)raw_offset;
    int64_t result = vfs_seek(handle, offset, (int)whence);
    return result < 0 ? (uint64_t)result : (uint64_t)result;
}

static uint64_t sys_getdents64(uint64_t fd, vaddr_t user_buffer, uint64_t count) {
    process_t *process = process_current();
    int handle = process && fd < NPK_MAX_FDS ? process_handle_for_fd(process, (int)fd) : -1;
    if (handle < 0) return neg_errno(EBADF);
    if (count == 0) return 0;
    if (count > 4096 || !vmm_is_user_range(user_buffer, count)) return neg_errno(EFAULT);
    uint8_t buffer[4096];
    ssize_t result = vfs_getdents64(handle, buffer, (size_t)count);
    if (result < 0) return (uint64_t)result;
    if (!vmm_copyout(user_buffer, buffer, (size_t)result)) return neg_errno(EFAULT);
    return (uint64_t)result;
}

static uint64_t sys_getcwd(vaddr_t user_buffer, uint64_t size) {
    static const char root[] = "/";
    if (size < sizeof(root)) return neg_errno(34); /* ERANGE */
    if (!vmm_is_user_range(user_buffer, sizeof(root))) return neg_errno(EFAULT);
    if (!vmm_copyout(user_buffer, root, sizeof(root))) return neg_errno(EFAULT);
    /* Raw Linux getcwd returns the string length, excluding the NUL. */
    return sizeof(root) - 1;
}

static uint64_t sys_read(uint64_t fd, vaddr_t user_buffer, uint64_t count) {
    process_t *process = process_current();
    int handle = process && fd < NPK_MAX_FDS ? process_handle_for_fd(process, (int)fd) : -1;
    if (handle < 0) return neg_errno(EBADF);
    if (!vmm_is_user_range(user_buffer, count)) return neg_errno(EFAULT);
    if (count == 0) return 0;
    char chunk[IO_CHUNK];
    uint64_t total = 0;
    while (total < count) {
        uint64_t amount = count - total < sizeof(chunk) ? count - total : sizeof(chunk);
        if (handle == 0) {
            uint64_t read = 0;
            while (read < amount) {
                int c = keyboard_getchar();
                if (c < 0) break;
                chunk[read++] = (char)c;
                if (c == '\n') break;
            }
            if (!vmm_copyout(user_buffer + total, chunk, (size_t)read)) return neg_errno(EFAULT);
            total += read;
            break; /* A terminal read returns the available line fragment. */
        }
        ssize_t result = vfs_read(handle, chunk, (size_t)amount);
        if (result < 0) return total ? total : (uint64_t)result;
        if (result == 0) break;
        if (!vmm_copyout(user_buffer + total, chunk, (size_t)result)) return neg_errno(EFAULT);
        total += (uint64_t)result;
        if ((uint64_t)result < amount) break;
    }
    return total;
}

typedef struct {
    vaddr_t base;
    uint64_t length;
} npk_iovec_t;

static uint64_t sys_readv(uint64_t raw_fd, vaddr_t user_iov, uint64_t raw_count) {
    int64_t signed_count = (int64_t)raw_count;
    if (signed_count < 0 || (uint64_t)signed_count > IOV_MAX_NPK) return neg_errno(EINVAL);
    if (signed_count != 0 && !vmm_is_user_range(user_iov, (uint64_t)signed_count * sizeof(npk_iovec_t)))
        return neg_errno(EFAULT);
    uint64_t total = 0;
    for (uint64_t i = 0; i < (uint64_t)signed_count; ++i) {
        npk_iovec_t iov;
        if (!vmm_copyin(&iov, user_iov + i * sizeof(iov), sizeof(iov))) return neg_errno(EFAULT);
        if (iov.length > UINT64_MAX - total) return neg_errno(EOVERFLOW);
        uint64_t result = sys_read(raw_fd, iov.base, iov.length);
        if ((int64_t)result < 0) return total ? total : result;
        total += result;
        if (result != iov.length) break;
    }
    return total;
}

static uint64_t sys_writev(uint64_t raw_fd, vaddr_t user_iov, uint64_t raw_count) {
    int64_t signed_count = (int64_t)raw_count;
    if (signed_count < 0 || (uint64_t)signed_count > IOV_MAX_NPK) return neg_errno(EINVAL);
    if (signed_count != 0 && !vmm_is_user_range(user_iov, (uint64_t)signed_count * sizeof(npk_iovec_t)))
        return neg_errno(EFAULT);
    uint64_t total = 0;
    for (uint64_t i = 0; i < (uint64_t)signed_count; ++i) {
        npk_iovec_t iov;
        if (!vmm_copyin(&iov, user_iov + i * sizeof(iov), sizeof(iov))) return neg_errno(EFAULT);
        if (iov.length > UINT64_MAX - total) return neg_errno(EOVERFLOW);
        uint64_t result = sys_write(raw_fd, iov.base, iov.length);
        if ((int64_t)result < 0) return total ? total : result;
        total += result;
        if (result != iov.length) break;
    }
    return total;
}

static uint64_t sys_uname(vaddr_t user_buffer) {
    npk_utsname_t uname;
    memset(&uname, 0, sizeof(uname));
    const char *values[] = { "NPKernel", "npk", "0.2.0-dev", "No Problem Kernel", "x86_64", "" };
    char *fields[] = { uname.sysname, uname.nodename, uname.release, uname.version, uname.machine, uname.domainname };
    for (unsigned i = 0; i < 6; ++i) {
        size_t length = strlen(values[i]);
        if (length > 64) length = 64;
        memcpy(fields[i], values[i], length);
    }
    if (!vmm_copyout(user_buffer, &uname, sizeof(uname))) return neg_errno(EFAULT);
    return 0;
}

static uint64_t sys_brk(uint64_t requested) {
    process_t *process = process_current();
    if (!process || !process->alive) return neg_errno(EFAULT);
    if (requested == 0) return process->brk_end;
    if (requested < process->brk_start || requested >= NPK_USER_MAX ||
        requested > UINT64_MAX - (NPK_PAGE_SIZE - 1)) return process->brk_end;
    uint64_t target = (requested + NPK_PAGE_SIZE - 1) & ~(NPK_PAGE_SIZE - 1);
    if (vm_resize_heap(process, target) != 0) return process->brk_end;
    return requested;
}

static int process_install_fd(process_t *process, int handle, int minimum, uint32_t flags) {
    if (!process || handle < 0 || minimum < 0 || minimum >= NPK_MAX_FDS) return -1;
    for (int i = minimum; i < NPK_MAX_FDS; ++i) {
        if (process->fds[i] != -1) continue;
        process->fds[i] = handle;
        process->fd_flags[i] = flags;
        return i;
    }
    return -1;
}

static int process_install_new_fd(process_t *process, int handle) {
    return process_install_fd(process, handle, 3, 0);
}

static int process_handle_for_fd(const process_t *process, int fd) {
    if (!process || fd < 0 || fd >= NPK_MAX_FDS) return -1;
    return process->fds[fd];
}

static void process_forget_fd(process_t *process, int fd) {
    if (!process || fd < 0 || fd >= NPK_MAX_FDS || process->fds[fd] < 0) return;
    (void)vfs_close(process->fds[fd]);
    process->fds[fd] = -1;
    process->fd_flags[fd] = 0;
}

static uint64_t sys_pipe_common(vaddr_t user_pipefd, uint64_t flags) {
    process_t *process = process_current();
    if (!process || !vmm_is_user_range(user_pipefd, sizeof(uint32_t) * 2)) return neg_errno(EFAULT);
    if (flags & ~(0x800ULL | 0x80000ULL)) return neg_errno(EINVAL); /* O_NONBLOCK/O_CLOEXEC. */
    int read_handle = -1, write_handle = -1;
    int result = vfs_pipe_create(&read_handle, &write_handle);
    if (result < 0) return (uint64_t)result;
    uint32_t fd_flags = (flags & 0x80000ULL) ? 1U : 0U; /* FD_CLOEXEC. */
    int read_fd = process_install_fd(process, read_handle, 3, fd_flags);
    if (read_fd < 0) {
        (void)vfs_close(read_handle); (void)vfs_close(write_handle);
        return neg_errno(EMFILE);
    }
    int write_fd = process_install_fd(process, write_handle, 3, fd_flags);
    if (write_fd < 0) {
        process_forget_fd(process, read_fd); (void)vfs_close(write_handle);
        return neg_errno(EMFILE);
    }
    uint32_t values[2] = {(uint32_t)read_fd, (uint32_t)write_fd};
    if (!vmm_copyout(user_pipefd, values, sizeof(values))) {
        process_forget_fd(process, read_fd); process_forget_fd(process, write_fd);
        return neg_errno(EFAULT);
    }
    return 0;
}

static uint64_t sys_dup(uint64_t raw_fd, int minimum, uint32_t flags) {
    process_t *process = process_current();
    if (!process || raw_fd >= NPK_MAX_FDS) return neg_errno(EBADF);
    int handle = process_handle_for_fd(process, (int)raw_fd);
    if (handle < 0 || vfs_retain(handle) != 0) return neg_errno(EBADF);
    int new_fd = process_install_fd(process, handle, minimum, flags);
    if (new_fd < 0) { (void)vfs_close(handle); return neg_errno(EMFILE); }
    return (uint64_t)new_fd;
}

static uint64_t sys_dup2(uint64_t raw_oldfd, uint64_t raw_newfd) {
    process_t *process = process_current();
    if (!process || raw_oldfd >= NPK_MAX_FDS || raw_newfd >= NPK_MAX_FDS) return neg_errno(EBADF);
    int oldfd = (int)raw_oldfd, newfd = (int)raw_newfd;
    int handle = process_handle_for_fd(process, oldfd);
    if (handle < 0) return neg_errno(EBADF);
    if (oldfd == newfd) return newfd;
    if (vfs_retain(handle) != 0) return neg_errno(EBADF);
    if (process->fds[newfd] >= 0) process_forget_fd(process, newfd);
    process->fds[newfd] = handle;
    process->fd_flags[newfd] = 0;
    return newfd;
}

static uint64_t sys_fcntl(uint64_t raw_fd, uint64_t command, uint64_t argument) {
    process_t *process = process_current();
    if (!process || raw_fd >= NPK_MAX_FDS || process_handle_for_fd(process, (int)raw_fd) < 0) return neg_errno(EBADF);
    int fd = (int)raw_fd;
    enum { F_DUPFD = 0, F_GETFD = 1, F_SETFD = 2, F_GETFL = 3, F_SETFL = 4, F_DUPFD_CLOEXEC = 1030 };
    switch (command) {
        case F_DUPFD: return sys_dup(raw_fd, (int)argument, 0);
        case F_DUPFD_CLOEXEC: return sys_dup(raw_fd, (int)argument, 1U);
        case F_GETFD: return process->fd_flags[fd] & 1U;
        case F_SETFD: process->fd_flags[fd] = (uint32_t)argument & 1U; return 0;
        case F_GETFL: return 0x800ULL; /* VFS operations are non-blocking by design. */
        case F_SETFL: return 0;
        default: return neg_errno(EINVAL);
    }
}

static uint64_t sys_mprotect(vaddr_t address, uint64_t length, uint64_t prot) {
    process_t *process = process_current();
    if (!process || !process->alive || !vmm_is_user_range(address, length)) return neg_errno(EFAULT);
    int result = vm_protect(process, address, length, prot);
    return result < 0 ? (uint64_t)result : 0;
}

static uint64_t sys_mmap(vaddr_t requested, uint64_t length, uint64_t prot,
                         uint64_t flags, uint64_t fd, uint64_t offset) {
    process_t *process = process_current();
    if (!process || !process->alive || length == 0 || (prot & ~7ULL) ||
        ((prot & NPK_PROT_WRITE) && (prot & NPK_PROT_EXEC)) ||
        (flags & (NPK_MAP_SHARED | NPK_MAP_PRIVATE)) == 0)
        return neg_errno(EINVAL);
    vaddr_t address = 0;
    int result;
    if (flags & NPK_MAP_ANONYMOUS) {
        if (fd != UINT64_MAX || offset != 0)
            return neg_errno(EINVAL);
        result = vm_map_anonymous(process, requested, length, prot, flags, &address);
    } else {
        if (fd >= NPK_MAX_FDS || (offset & (NPK_PAGE_SIZE - 1)) != 0)
            return neg_errno(EINVAL);
        int handle = process_handle_for_fd(process, (int)fd);
        if (handle < 0) return neg_errno(EBADF);
        result = vfs_is_shared_memory(handle) ?
                 vm_map_shared(process, handle, requested, length, prot, flags, offset, &address) :
                 vm_map_file(process, handle, requested, length, prot, flags, offset, &address);
    }
    if (result < 0) return (uint64_t)result;
    return address;
}

static uint64_t sys_wait4(uint64_t raw_pid, vaddr_t user_status, uint64_t options) {
    int64_t pid = (int64_t)raw_pid;
    int64_t result = process_wait4(pid, user_status, options);
    return (uint64_t)result;
}

static uint64_t sys_sched_yield(void) {
    scheduler_yield();
    return 0;
}

static bool valid_user_base(uint64_t base) {
    return base == 0 || (base <= 0x00007fffffffffffULL);
}

static uint64_t sys_arch_prctl(uint64_t operation, uint64_t argument) {
    thread_t *thread = scheduler_current();
    process_t *process = process_current();
    if (!thread || !process || !process->alive || thread->privilege_ring != 3) return neg_errno(EPERM);
    switch (operation) {
        case ARCH_SET_FS:
            if (!valid_user_base(argument)) return neg_errno(EINVAL);
            thread->fs_base = argument;
            arch_set_fs_base(argument);
            return 0;
        case ARCH_GET_FS: {
            uint64_t base = thread->fs_base;
            return vmm_copyout((vaddr_t)argument, &base, sizeof(base)) ? 0 : neg_errno(EFAULT);
        }
        case ARCH_SET_GS:
            if (!valid_user_base(argument)) return neg_errno(EINVAL);
            thread->gs_base = argument;
            arch_set_user_gs_base(argument);
            return 0;
        case ARCH_GET_GS: {
            uint64_t base = thread->gs_base;
            return vmm_copyout((vaddr_t)argument, &base, sizeof(base)) ? 0 : neg_errno(EFAULT);
        }
        default:
            return neg_errno(EINVAL);
    }
}

static uint64_t sys_rt_sigaction(uint64_t raw_signal, vaddr_t user_new_action,
                                 vaddr_t user_old_action, uint64_t raw_sigset_size) {
    uint64_t signal = raw_signal;
    process_t *process = process_current();
    if (!process || !process->alive || signal == 0 || signal > NPK_SIGNAL_MAX || signal == 9 || signal == 19)
        return neg_errno(EINVAL);
    /* x86_64 Linux uses an eight-byte kernel sigset; accept the larger libc
     * representation too, while copying the complete user action safely. */
    if (raw_sigset_size != 0 && raw_sigset_size != 8 && raw_sigset_size != 128)
        return neg_errno(EINVAL);
    if (user_old_action && !vmm_copyout(user_old_action, &process->signal_actions[signal], sizeof(npk_sigaction_t)))
        return neg_errno(EFAULT);
    if (user_new_action) {
        npk_sigaction_t action;
        if (!vmm_copyin(&action, user_new_action, sizeof(action))) return neg_errno(EFAULT);
        process->signal_actions[signal] = action;
    }
    return 0;
}

static uint64_t sys_rt_sigprocmask(uint64_t operation, vaddr_t user_set,
                                     vaddr_t user_old_set, uint64_t raw_sigset_size) {
    enum { SIG_BLOCK = 0, SIG_UNBLOCK = 1, SIG_SETMASK = 2 };
    if (raw_sigset_size != 8 && raw_sigset_size != 128) return neg_errno(EINVAL);
    if (operation > SIG_SETMASK) return neg_errno(EINVAL);

    uint64_t old_mask = process_get_signal_mask();
    if (user_old_set) {
        uint64_t old_set[16] = {0};
        old_set[0] = old_mask;
        if (!vmm_copyout(user_old_set, old_set, raw_sigset_size)) return neg_errno(EFAULT);
    }
    if (user_set) {
        uint64_t new_set[16] = {0};
        if (!vmm_copyin(new_set, user_set, raw_sigset_size)) return neg_errno(EFAULT);
        uint64_t mask = new_set[0];
        if (operation == SIG_BLOCK) process_set_signal_mask(old_mask | mask);
        else if (operation == SIG_UNBLOCK) process_set_signal_mask(old_mask & ~mask);
        else process_set_signal_mask(mask);
    }
    return 0;
}

static uint64_t sys_rt_sigqueueinfo(int64_t pid, uint64_t signal, vaddr_t user_info) {
    if (signal < NPK_SIGRTMIN || signal > NPK_SIGRTMAX || !user_info) return neg_errno(EINVAL);
    uint8_t raw[128] = {0};
    if (!vmm_copyin(raw, user_info, sizeof(raw))) return neg_errno(EFAULT);
    int32_t signo = 0;
    int32_t code = 0;
    int32_t sender_pid = 0;
    uint32_t sender_uid = 0;
    int64_t value = 0;
    memcpy(&signo, raw + 0, sizeof(signo));
    memcpy(&code, raw + 8, sizeof(code));
    memcpy(&sender_pid, raw + 16, sizeof(sender_pid));
    memcpy(&sender_uid, raw + 20, sizeof(sender_uid));
    memcpy(&value, raw + 24, sizeof(value));
    if (signo != (int32_t)signal) return neg_errno(EINVAL);
    process_t *sender = process_current();
    if (!sender || !sender->alive) return neg_errno(ESRCH);
    npk_signal_info_t info = { .signal = signal, .code = code, .sender_pid = (int64_t)sender->pid,
                               .sender_uid = 0, .value = value };
    (void)sender_pid;
    (void)sender_uid;
    return (uint64_t)process_send_signal_info(pid, signal, &info);
}

static uint64_t sys_rt_tgsigqueueinfo(int64_t tgid, int64_t tid, uint64_t signal,
                                      vaddr_t user_info) {
    if (signal < NPK_SIGRTMIN || signal > NPK_SIGRTMAX || !user_info) return neg_errno(EINVAL);
    uint8_t raw[128] = {0};
    if (!vmm_copyin(raw, user_info, sizeof(raw))) return neg_errno(EFAULT);
    int32_t signo = 0;
    int32_t code = 0;
    int32_t sender_pid = 0;
    uint32_t sender_uid = 0;
    int64_t value = 0;
    memcpy(&signo, raw + 0, sizeof(signo));
    memcpy(&code, raw + 8, sizeof(code));
    memcpy(&sender_pid, raw + 16, sizeof(sender_pid));
    memcpy(&sender_uid, raw + 20, sizeof(sender_uid));
    memcpy(&value, raw + 24, sizeof(value));
    if (signo != (int32_t)signal) return neg_errno(EINVAL);
    process_t *sender = process_current();
    if (!sender || !sender->alive) return neg_errno(ESRCH);
    npk_signal_info_t info = { .signal = signal, .code = code, .sender_pid = (int64_t)sender->pid,
                               .sender_uid = 0, .value = value };
    (void)sender_pid;
    (void)sender_uid;
    return (uint64_t)process_send_thread_signal_info(tgid, tid, signal, &info);
}

static uint64_t sys_clock_gettime(uint64_t clock_id, vaddr_t user_timespec) {
    if (clock_id > 1) return neg_errno(EINVAL);
    uint64_t ns = timer_monotonic_ns();
    npk_timespec_t value = { .seconds = (int64_t)(ns / 1000000000ULL), .nanoseconds = (int64_t)(ns % 1000000000ULL) };
    return vmm_copyout(user_timespec, &value, sizeof(value)) ? 0 : neg_errno(EFAULT);
}

static uint64_t sys_futex(vaddr_t user_address, uint64_t operation, uint32_t value,
                            vaddr_t user_timeout, vaddr_t user_address2, uint32_t value3) {
    enum { FUTEX_WAIT = 0, FUTEX_WAKE = 1, FUTEX_PRIVATE_FLAG = 128, FUTEX_CLOCK_REALTIME = 256 };
    (void)user_address2;
    (void)value3;
    if (operation & FUTEX_CLOCK_REALTIME) return neg_errno(EINVAL);
    if (operation & ~(uint64_t)(0x7fU | FUTEX_PRIVATE_FLAG)) return neg_errno(EINVAL);
    uint64_t command = operation & 0x7fU;
    if (command == FUTEX_WAIT)
        return (uint64_t)process_futex_wait(user_address, value, user_timeout);
    if (command == FUTEX_WAKE)
        return (uint64_t)process_futex_wake(user_address, value);
    return neg_errno(ENOSYS);
}

typedef struct {
    int32_t fd;
    int16_t events;
    int16_t revents;
} npk_pollfd_t;

typedef struct {
    uint32_t events;
    uint32_t padding;
    uint64_t data;
} npk_epoll_event_t;

#define NPK_POLLNVAL 0x020
#define NPK_POLL_MAX_FDS NPK_MAX_FDS

static int64_t poll_timeout_ms(vaddr_t user_timeout, bool *valid) {
    if (valid) *valid = false;
    if (!user_timeout) {
        if (valid) *valid = true;
        return -1;
    }
    npk_timespec_t timeout;
    if (!vmm_copyin(&timeout, user_timeout, sizeof(timeout)) || timeout.seconds < 0 ||
        timeout.nanoseconds < 0 || timeout.nanoseconds >= 1000000000LL) return 0;
    if ((uint64_t)timeout.seconds > (uint64_t)INT64_MAX / 1000ULL) return 0;
    int64_t milliseconds = timeout.seconds * 1000 + timeout.nanoseconds / 1000000;
    if (timeout.nanoseconds % 1000000 != 0) ++milliseconds;
    if (valid) *valid = true;
    return milliseconds;
}

static int poll_scan(npk_pollfd_t *pollfds, size_t count) {
    int ready = 0;
    for (size_t i = 0; i < count; ++i) {
        pollfds[i].revents = 0;
        if (pollfds[i].fd < 0) continue;
        process_t *process = process_current();
        int handle = process && pollfds[i].fd < NPK_MAX_FDS ? process_handle_for_fd(process, pollfds[i].fd) : -1;
        if (handle < 0) {
            pollfds[i].revents = NPK_POLLNVAL;
        } else {
            pollfds[i].revents = (int16_t)vfs_poll_events(handle, (uint32_t)(uint16_t)pollfds[i].events);
        }
        if (pollfds[i].revents) ++ready;
    }
    return ready;
}

static uint64_t sys_poll_common(vaddr_t user_pollfds, uint64_t raw_count, int64_t timeout_ms) {
    if (raw_count > NPK_POLL_MAX_FDS || (raw_count && !vmm_is_user_range(user_pollfds, raw_count * sizeof(npk_pollfd_t))))
        return neg_errno(EFAULT);
    npk_pollfd_t pollfds[NPK_POLL_MAX_FDS];
    size_t count = (size_t)raw_count;
    if (count && !vmm_copyin(pollfds, user_pollfds, count * sizeof(*pollfds))) return neg_errno(EFAULT);
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        uint64_t ticks = ((uint64_t)timeout_ms * timer_frequency() + 999ULL) / 1000ULL;
        if (ticks == 0) ticks = 1;
        deadline = timer_ticks() + ticks;
    }
    for (;;) {
        int ready = poll_scan(pollfds, count);
        if (ready || timeout_ms == 0 || (timeout_ms > 0 && (int64_t)(timer_ticks() - deadline) >= 0)) {
            if (count && !vmm_copyout(user_pollfds, pollfds, count * sizeof(*pollfds))) return neg_errno(EFAULT);
            return (uint64_t)ready;
        }
        /* The timer interrupt supplies a bounded sleep interval, while other
         * threads and keyboard IRQs can make a descriptor ready between scans. */
        timer_sleep_ticks(1);
    }
}

static uint64_t sys_poll(vaddr_t user_pollfds, uint64_t raw_count, int64_t timeout_ms) {
    if (timeout_ms < -1) return neg_errno(EINVAL);
    return sys_poll_common(user_pollfds, raw_count, timeout_ms);
}

static uint64_t sys_ppoll(vaddr_t user_pollfds, uint64_t raw_count, vaddr_t user_timeout,
                          vaddr_t user_sigmask, uint64_t sigset_size) {
    if (user_sigmask && sigset_size != 8 && sigset_size != 128) return neg_errno(EINVAL);
    uint64_t old_mask = process_get_signal_mask();
    if (user_sigmask) {
        uint64_t mask[16] = {0};
        if (!vmm_copyin(mask, user_sigmask, sigset_size)) return neg_errno(EFAULT);
        process_set_signal_mask(mask[0]);
    }
    bool valid = false;
    int64_t timeout_ms = poll_timeout_ms(user_timeout, &valid);
    uint64_t result = valid ? sys_poll_common(user_pollfds, raw_count, timeout_ms) : neg_errno(EFAULT);
    if (user_sigmask) process_set_signal_mask(old_mask);
    return result;
}

static uint64_t sys_epoll_create1(uint64_t flags) {
    if (flags & ~0x80000ULL) return neg_errno(EINVAL);
    process_t *process = process_current();
    if (!process) return neg_errno(EFAULT);
    int handle = vfs_epoll_create();
    if (handle < 0) return (uint64_t)handle;
    int fd = process_install_fd(process, handle, 3, (flags & 0x80000ULL) ? 1U : 0U);
    if (fd < 0) {
        (void)vfs_close(handle);
        return neg_errno(EMFILE);
    }
    return (uint64_t)fd;
}

static uint64_t sys_epoll_ctl(uint64_t raw_epfd, uint64_t operation, uint64_t raw_fd, vaddr_t user_event) {
    process_t *process = process_current();
    if (!process || raw_epfd >= NPK_MAX_FDS || raw_fd >= NPK_MAX_FDS ||
        !vmm_is_user_range(user_event, sizeof(npk_epoll_event_t))) return neg_errno(EFAULT);
    int epfd = process_handle_for_fd(process, (int)raw_epfd);
    int fd = process_handle_for_fd(process, (int)raw_fd);
    if (epfd < 0 || fd < 0) return neg_errno(EBADF);
    npk_epoll_event_t event;
    if (!vmm_copyin(&event, user_event, sizeof(event))) return neg_errno(EFAULT);
    return (uint64_t)vfs_epoll_ctl(epfd, (int)operation, fd, event.events, event.data);
}

static uint64_t sys_epoll_wait(uint64_t raw_epfd, vaddr_t user_events, uint64_t maxevents, int64_t timeout_ms) {
    if (maxevents == 0 || maxevents > 32 || timeout_ms < -1) return neg_errno(EINVAL);
    process_t *process = process_current();
    if (!process || raw_epfd >= NPK_MAX_FDS || !vmm_is_user_range(user_events, maxevents * sizeof(npk_epoll_event_t)))
        return neg_errno(EFAULT);
    int epfd = process_handle_for_fd(process, (int)raw_epfd);
    if (epfd < 0) return neg_errno(EBADF);
    uint64_t deadline = 0;
    if (timeout_ms > 0) {
        uint64_t ticks = ((uint64_t)timeout_ms * timer_frequency() + 999ULL) / 1000ULL;
        if (ticks == 0) ticks = 1;
        deadline = timer_ticks() + ticks;
    }
    vfs_epoll_event_t ready[32];
    for (;;) {
        ssize_t count = vfs_epoll_wait(epfd, ready, (size_t)maxevents);
        if (count < 0) return (uint64_t)count;
        if (count != 0 || timeout_ms == 0 || (timeout_ms > 0 && (int64_t)(timer_ticks() - deadline) >= 0)) {
            for (ssize_t i = 0; i < count; ++i) {
                npk_epoll_event_t output = { .events = ready[i].events, .padding = 0, .data = ready[i].data };
                if (!vmm_copyout(user_events + (size_t)i * sizeof(output), &output, sizeof(output))) return neg_errno(EFAULT);
            }
            return (uint64_t)count;
        }
        timer_sleep_ticks(1);
    }
}

static uint64_t sys_nanosleep(uint64_t user_request, uint64_t user_remainder) {
    npk_timespec_t request;
    if (!vmm_copyin(&request, user_request, sizeof(request)) || request.seconds < 0 || request.nanoseconds < 0 || request.nanoseconds >= 1000000000LL) return neg_errno(EFAULT);
    uint64_t ticks = (uint64_t)request.seconds * timer_frequency() + ((uint64_t)request.nanoseconds * timer_frequency()) / 1000000000ULL;
    if (ticks == 0 && (request.seconds != 0 || request.nanoseconds != 0)) ticks = 1;
    timer_sleep_ticks(ticks);
    if (user_remainder) {
        npk_timespec_t zero = {0, 0};
        if (!vmm_copyout(user_remainder, &zero, sizeof(zero))) return neg_errno(EFAULT);
    }
    return 0;
}

#define EXEC_MAX_VECTOR 32U
#define EXEC_STRING_STORAGE 4096U

static bool copy_user_vector(vaddr_t user_vector, const char **kernel_vector, size_t capacity,
                             char *storage, size_t storage_capacity, size_t *count, size_t *used) {
    if (kernel_vector == NULL || storage == NULL || count == NULL || used == NULL) return false;
    *count = 0;
    if (user_vector == 0) return true;
    for (size_t i = 0; i < capacity; ++i) {
        if (i > (UINT64_MAX - user_vector) / sizeof(vaddr_t)) return false;
        vaddr_t pointer_address = user_vector + i * sizeof(vaddr_t);
        vaddr_t user_string;
        if (!vmm_copyin(&user_string, pointer_address, sizeof(user_string))) return false;
        if (user_string == 0) {
            *count = i;
            return true;
        }
        if (*used >= storage_capacity || !copy_user_string(user_string, storage + *used, storage_capacity - *used)) return false;
        kernel_vector[i] = storage + *used;
        *used += strlen(storage + *used) + 1;
    }
    return false;
}

static uint64_t sys_execve(syscall_frame_t *frame) {
    if (!frame) return neg_errno(EFAULT);
    vaddr_t user_path = frame->rdi;
    vaddr_t user_argv = frame->rsi;
    vaddr_t user_envp = frame->rdx;
    char path[PATH_MAX_NPK];
    if (!copy_user_string(user_path, path, sizeof(path))) return neg_errno(user_path ? ENAMETOOLONG : EFAULT);
    int fd = vfs_open(path);
    if (fd < 0) return neg_errno(ENOENT);
    ssize_t image_size = vfs_size(fd);
    if (image_size <= 0 || (uint64_t)image_size > EXEC_MAX_IMAGE) { vfs_close(fd); return neg_errno(EINVAL); }
    uint8_t *image = (uint8_t *)kmalloc((size_t)image_size);
    if (!image) { vfs_close(fd); return neg_errno(ENOMEM); }
    ssize_t read = vfs_read(fd, image, (size_t)image_size);
    vfs_close(fd);
    if (read != image_size) { kfree(image); return neg_errno(EFAULT); }
    const char *argv[EXEC_MAX_VECTOR];
    const char *envp[EXEC_MAX_VECTOR];
    char argument_storage[EXEC_STRING_STORAGE];
    size_t argc = 0, envc = 0, used = 0;
    bool vectors_ok = copy_user_vector(user_argv, argv, EXEC_MAX_VECTOR, argument_storage, sizeof(argument_storage), &argc, &used) &&
                      copy_user_vector(user_envp, envp, EXEC_MAX_VECTOR, argument_storage, sizeof(argument_storage), &envc, &used);
    if (!vectors_ok) {
        kfree(image);
        return neg_errno(EFAULT);
    }
    int64_t result = process_exec_current(image, (size_t)image_size, argv, argc,
                                          envp, envc, frame);
    if (result == 0) process_set_image_path(process_current(), path);
    kfree(image);
    return (uint64_t)result;
}

static uint64_t sys_open_path(vaddr_t user_path, uint64_t raw_flags, uint64_t raw_mode) {
    process_t *process = process_current();
    if (!process) return neg_errno(EFAULT);
    char path[PATH_MAX_NPK];
    if (!copy_user_string(user_path, path, sizeof(path))) return neg_errno(EFAULT);
    if (path[0] != '/') return neg_errno(EINVAL);
    int handle = vfs_open_flags(path, (uint32_t)raw_flags, (uint32_t)raw_mode);
    if (handle < 0) return (uint64_t)handle;
    int fd = process_install_new_fd(process, handle);
    if (fd < 0) {
        (void)vfs_close(handle);
        return neg_errno(EMFILE);
    }
    return (uint64_t)fd;
}

static uint64_t sys_openat_path(int64_t raw_dirfd, vaddr_t user_path,
                                uint64_t raw_flags, uint64_t raw_mode) {
    /* The current kernel has no cwd/name-resolution layer. Absolute paths and
     * AT_FDCWD are still useful for libc; relative directory traversal stays
     * rejected instead of being silently resolved against the wrong root. */
    if (raw_dirfd != -100)
        return neg_errno(EINVAL);
    return sys_open_path(user_path, raw_flags, raw_mode);
}

static uint64_t sys_shm_create(uint64_t raw_size) {
    process_t *process = process_current();
    if (!process || raw_size == 0 || raw_size > (256ULL * NPK_PAGE_SIZE)) return neg_errno(EINVAL);
    int handle = vfs_shm_create((size_t)raw_size);
    if (handle < 0) return (uint64_t)handle;
    int fd = process_install_new_fd(process, handle);
    if (fd < 0) {
        (void)vfs_close(handle);
        return neg_errno(EMFILE);
    }
    return (uint64_t)fd;
}

static uint64_t sys_fd_send(uint64_t raw_pid, uint64_t raw_fd) {
    if (raw_pid > INT64_MAX || raw_fd >= NPK_MAX_FDS) return neg_errno(EINVAL);
    return (uint64_t)process_send_fd((int64_t)raw_pid, (int)raw_fd);
}

static uint64_t sys_fd_recv(void) {
    process_t *process = process_current();
    if (!process) return neg_errno(EFAULT);
    int handle = -1;
    uint32_t fd_flags = 0;
    int result = process_receive_fd(process, &handle, &fd_flags);
    if (result < 0) return (uint64_t)result;
    int fd = process_install_fd(process, handle, 3, fd_flags);
    if (fd < 0) {
        (void)vfs_close(handle);
        return neg_errno(EMFILE);
    }
    return (uint64_t)fd;
}

static uint64_t sys_display_info(vaddr_t user_info) {
    gop_framebuffer_info_t info;
    if (!gop_get_framebuffer_info(&info)) return neg_errno(ENODEV);
    return vmm_copyout(user_info, &info, sizeof(info)) ? 0 : neg_errno(EFAULT);
}

static uint64_t sys_display_claim(void) {
    process_t *process = process_current();
    if (!process || !process->alive) return neg_errno(EFAULT);
    if (!gop_claim_user_display(process->pid)) return neg_errno(ENODEV);
    keyboard_clear_events();
    return 0;
}

static uint64_t sys_display_map(vaddr_t requested, uint64_t flags) {
    process_t *process = process_current();
    if (!process || !process->alive || !gop_user_display_owned_by(process->pid)) return neg_errno(EPERM);
    if ((flags & NPK_MAP_SHARED) == 0 || (flags & NPK_MAP_PRIVATE) != 0 ||
        (flags & NPK_MAP_ANONYMOUS) != 0) return neg_errno(EINVAL);
    gop_framebuffer_info_t info;
    if (!gop_get_framebuffer_info(&info)) return neg_errno(ENODEV);
    vaddr_t address = 0;
    int result = vm_map_device(process, requested, info.physical_base, info.mapping_size,
                               NPK_PROT_READ | NPK_PROT_WRITE, flags, &address);
    return result < 0 ? (uint64_t)result : address;
}

static uint64_t sys_display_release(void) {
    process_t *process = process_current();
    if (!process || !process->alive || !gop_user_display_owned_by(process->pid)) return neg_errno(EPERM);
    vm_revoke_device_mappings(process);
    gop_release_user_display(process->pid);
    return 0;
}

static uint64_t sys_input_read(vaddr_t user_buffer, uint64_t capacity) {
    process_t *process = process_current();
    if (!process || !process->alive || !gop_user_display_owned_by(process->pid)) return neg_errno(EPERM);
    if (capacity == 0) return 0;
    if (!vmm_is_user_range(user_buffer, capacity)) return neg_errno(EFAULT);
    npk_input_event_t events[16];
    uint64_t total = 0;
    while (total + sizeof(npk_input_event_t) <= capacity && total < sizeof(events)) {
        npk_input_event_t event;
        if (!keyboard_get_event(&event)) break;
        events[total / sizeof(npk_input_event_t)] = event;
        total += sizeof(npk_input_event_t);
    }
    if (total != 0 && !vmm_copyout(user_buffer, events, (size_t)total)) return neg_errno(EFAULT);
    return total;
}

static uint64_t sys_sync_fd(uint64_t raw_fd) {
    process_t *process = process_current();
    if (!process || raw_fd >= NPK_MAX_FDS) return neg_errno(EBADF);
    int handle = process_handle_for_fd(process, (int)raw_fd);
    if (handle < 0) return neg_errno(EBADF);
    int result = vfs_sync_fd(handle);
    return result < 0 ? (uint64_t)result : 0;
}

static bool terminal_fd_valid(process_t *process, uint64_t raw_fd) {
    if (!process || raw_fd >= NPK_MAX_FDS) return false;
    int fd = (int)raw_fd;
    int handle = process_handle_for_fd(process, fd);
    if (fd <= 2) return handle == fd;
    return handle >= 0;
}

static uint64_t sys_ioctl(uint64_t raw_fd, uint64_t request, vaddr_t user_arg) {
    process_t *process = process_current();
    if (!terminal_fd_valid(process, raw_fd)) return neg_errno(EBADF);
    if (raw_fd > 2) return neg_errno(ENOTTY);
    switch (request) {
        case NPK_IOCTL_TCGETS:
            if (!vmm_copyout(user_arg, &terminal_settings, sizeof(terminal_settings))) return neg_errno(EFAULT);
            return 0;
        case NPK_IOCTL_TCSETS:
        case NPK_IOCTL_TCSETSW:
        case NPK_IOCTL_TCSETSF: {
            npk_termios_t settings;
            if (!vmm_copyin(&settings, user_arg, sizeof(settings))) return neg_errno(EFAULT);
            terminal_settings = settings;
            return 0;
        }
        case NPK_IOCTL_TIOCGWINSZ:
            if (!gop_ready()) {
                terminal_winsize.rows = 25;
                terminal_winsize.columns = 80;
                terminal_winsize.x_pixels = 0;
                terminal_winsize.y_pixels = 0;
            } else {
                uint64_t rows = gop_rows();
                uint64_t columns = gop_columns();
                uint64_t width = gop_width();
                uint64_t height = gop_height();
                terminal_winsize.rows = (uint16_t)(rows > UINT16_MAX ? UINT16_MAX : rows);
                terminal_winsize.columns = (uint16_t)(columns > UINT16_MAX ? UINT16_MAX : columns);
                terminal_winsize.x_pixels = (uint16_t)(width > UINT16_MAX ? UINT16_MAX : width);
                terminal_winsize.y_pixels = (uint16_t)(height > UINT16_MAX ? UINT16_MAX : height);
            }
            if (!vmm_copyout(user_arg, &terminal_winsize, sizeof(terminal_winsize))) return neg_errno(EFAULT);
            return 0;
        case NPK_IOCTL_TIOCSWINSZ: {
            npk_winsize_t size;
            if (!vmm_copyin(&size, user_arg, sizeof(size))) return neg_errno(EFAULT);
            terminal_winsize = size;
            return 0;
        }
        case NPK_IOCTL_FIONREAD: {
            if (raw_fd != 0) return neg_errno(ENOTTY);
            uint32_t available = keyboard_has_data() ? 1U : 0U;
            if (!vmm_copyout(user_arg, &available, sizeof(available))) return neg_errno(EFAULT);
            return 0;
        }
        default:
            return neg_errno(ENOTTY);
    }
}

static uint64_t sys_readlink_path(vaddr_t user_path, vaddr_t user_buffer, uint64_t capacity) {
    process_t *process = process_current();
    if (!process || capacity == 0 || !vmm_is_user_range(user_buffer, capacity)) return neg_errno(EINVAL);
    char path[PATH_MAX_NPK];
    if (!copy_user_string(user_path, path, sizeof(path))) return neg_errno(EFAULT);
    const char *target = NULL;
    if (strcmp(path, "/proc/self/exe") == 0 && process->image_path[0] != '\0')
        target = process->image_path;
    if (!target) return neg_errno(ENOENT);
    size_t length = strlen(target);
    if (length > capacity) length = (size_t)capacity;
    if (!vmm_copyout(user_buffer, target, length)) return neg_errno(EFAULT);
    return length;
}

void syscall_init(void) {
    LOG_INFOF("syscall", "Linux x86_64 ABI table entries", 56);
    log_message(LOG_INFO, "syscall", "safe pointers, metadata, mmap/mprotect/munmap, ioctl/readlink, signals, brk, proc, timer, execve and NPK IPC registered");
}

void syscall_cpu_init(void) {
    uint64_t efer = rdmsr(0xc0000080);
    wrmsr(0xc0000080, efer | 1ULL);
    wrmsr(0xc0000081, ((uint64_t)0x000b << 48) | ((uint64_t)0x0008 << 32));
    wrmsr(0xc0000082, (uint64_t)syscall_entry);
    wrmsr(0xc0000084, (1ULL << 9) | (1ULL << 10) | (1ULL << 8)); /* mask IF, DF, TF on kernel entry */
}

uint64_t syscall_dispatch(syscall_frame_t *frame) {
    if (frame == NULL) return neg_errno(EFAULT);
    LOG_INFOF("syscall", "user syscall number", frame->rax);
    switch (frame->rax) {
        case NPK_SYS_READ: return sys_read(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_WRITE: return sys_write(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_READV: return sys_readv(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_WRITEV: return sys_writev(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_STAT: return sys_stat_path(frame->rdi, frame->rsi);
        case NPK_SYS_FSTAT: return sys_fstat(frame->rdi, frame->rsi);
        case NPK_SYS_LSEEK: return sys_lseek(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_GETCWD: return sys_getcwd(frame->rdi, frame->rsi);
        case NPK_SYS_GETDENTS64: return sys_getdents64(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_POLL: return sys_poll(frame->rdi, frame->rsi, (int64_t)frame->rdx);
        case NPK_SYS_PPOLL: return sys_ppoll(frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8);
        case NPK_SYS_EPOLL_CREATE1: return sys_epoll_create1(frame->rdi);
        case NPK_SYS_EPOLL_CTL: return sys_epoll_ctl(frame->rdi, frame->rsi, frame->rdx, frame->r10);
        case NPK_SYS_EPOLL_WAIT: return sys_epoll_wait(frame->rdi, frame->rsi, frame->rdx, (int64_t)frame->r10);
        case NPK_SYS_OPEN:
            return sys_open_path(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_OPENAT:
            return sys_openat_path((int64_t)frame->rdi, frame->rsi, frame->rdx, frame->r10);
        case NPK_SYS_IOCTL: return sys_ioctl(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_READLINK: return sys_readlink_path(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_NPK_SHM_CREATE: return sys_shm_create(frame->rdi);
        case NPK_SYS_NPK_DISPLAY_INFO: return sys_display_info(frame->rdi);
        case NPK_SYS_NPK_DISPLAY_CLAIM: return sys_display_claim();
        case NPK_SYS_NPK_DISPLAY_MAP: return sys_display_map(frame->rdi, frame->rsi);
        case NPK_SYS_NPK_DISPLAY_RELEASE: return sys_display_release();
        case NPK_SYS_NPK_INPUT_READ: return sys_input_read(frame->rdi, frame->rsi);
        case NPK_SYS_NPK_FD_SEND: return sys_fd_send(frame->rdi, frame->rsi);
        case NPK_SYS_NPK_FD_RECV: return sys_fd_recv();
        case NPK_SYS_FSYNC:
        case NPK_SYS_FDATASYNC:
            return sys_sync_fd(frame->rdi);
        case NPK_SYS_SYNC: {
            int result = vfs_sync_all();
            return result < 0 ? (uint64_t)result : 0;
        }
        case NPK_SYS_CLOSE: {
            process_t *process = process_current();
            int fd = (int)frame->rdi;
            if (!process || fd < 0 || fd >= NPK_MAX_FDS || process_handle_for_fd(process, fd) < 0)
                return neg_errno(EBADF);
            process_forget_fd(process, fd);
            return 0;
        }
        case NPK_SYS_PIPE: return sys_pipe_common(frame->rdi, 0);
        case NPK_SYS_DUP: return sys_dup(frame->rdi, 3, 0);
        case NPK_SYS_DUP2: return sys_dup2(frame->rdi, frame->rsi);
        case NPK_SYS_FCNTL: return sys_fcntl(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_FUTEX: {
            uint64_t result = sys_futex(frame->rdi, frame->rsi, (uint32_t)frame->rdx,
                                        frame->r10, frame->r8, (uint32_t)frame->r9);
            if ((int64_t)result == -EINTR) (void)process_deliver_pending_signal_syscall(frame);
            return result;
        }
        case NPK_SYS_GETPID: return current_process ? current_process->pid : 1;
        case NPK_SYS_GETTID: return scheduler_current() ? scheduler_current()->tid : 1;
        case NPK_SYS_SET_TID_ADDRESS: return (uint64_t)process_set_tid_address(frame->rdi);
        case NPK_SYS_UNAME: return sys_uname(frame->rdi);
        case NPK_SYS_MMAP: return sys_mmap(frame->rdi, frame->rsi, frame->rdx, frame->r10, frame->r8, frame->r9);
        case NPK_SYS_MPROTECT: return sys_mprotect(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_MUNMAP: return (uint64_t)vm_unmap(process_current(), frame->rdi, frame->rsi);
        case NPK_SYS_BRK: return sys_brk(frame->rdi);
        case NPK_SYS_NANOSLEEP: return sys_nanosleep(frame->rdi, frame->rsi);
        case NPK_SYS_CLOCK_GETTIME: return sys_clock_gettime(frame->rdi, frame->rsi);
        case NPK_SYS_EXECVE: return sys_execve(frame);
        case NPK_SYS_EXIT:
            process_exit_thread_current((uint8_t)frame->rdi);
            log_message(LOG_INFO, "syscall", "thread exit syscall completed; yielding to scheduler");
            /* An exiting thread must never return through SYSRET. */
            scheduler_yield();
            arch_halt();
        case NPK_SYS_EXIT_GROUP:
            process_exit_current((uint8_t)frame->rdi);
            log_message(LOG_INFO, "syscall", "exit syscall completed; yielding to scheduler");
            /* The exiting thread must never return through SYSRET. If another
             * runnable thread exists, context_switch transfers control there;
             * otherwise remain safely in ring 0 rather than re-entering user code. */
            scheduler_yield();
            arch_halt();
        case NPK_SYS_FORK: return (uint64_t)process_fork(frame);
        case NPK_SYS_CLONE:
            return (uint64_t)process_clone(frame->rdi, frame->rsi, frame->rdx,
                                           frame->r10, frame->r8, frame);
        case NPK_SYS_WAIT4: return sys_wait4(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_WAITPID: return sys_wait4(frame->rdi, frame->rsi, frame->rdx);
        case NPK_SYS_SCHED_YIELD: return sys_sched_yield();
        case NPK_SYS_RT_SIGRETURN: {
            uint64_t result = process_sigreturn(frame);
            if (scheduler_current() && !scheduler_current()->signal_inflight)
                (void)process_deliver_pending_signal_syscall(frame);
            return result;
        }
        case NPK_SYS_ARCH_PRCTL: return sys_arch_prctl(frame->rdi, frame->rsi);
        case NPK_SYS_RT_SIGACTION:
            return sys_rt_sigaction(frame->rdi, frame->rsi, frame->rdx, frame->r10);
        case NPK_SYS_RT_SIGQUEUEINFO: {
            uint64_t result = sys_rt_sigqueueinfo((int64_t)frame->rdi, frame->rsi, frame->rdx);
            if ((int64_t)result == 0) (void)process_deliver_pending_signal_syscall(frame);
            return result;
        }
        case NPK_SYS_RT_TGSIGQUEUEINFO: {
            uint64_t result = sys_rt_tgsigqueueinfo((int64_t)frame->rdi, (int64_t)frame->rsi, frame->rdx, frame->r10);
            if ((int64_t)result == 0) (void)process_deliver_pending_signal_syscall(frame);
            return result;
        }
        case NPK_SYS_RT_SIGPROCMASK: {
            uint64_t result = sys_rt_sigprocmask(frame->rdi, frame->rsi, frame->rdx, frame->r10);
            if ((int64_t)result == 0) (void)process_deliver_pending_signal_syscall(frame);
            return result;
        }
        case NPK_SYS_KILL: {
            int64_t result = process_send_signal((int64_t)frame->rdi, frame->rsi);
            if (result == 0) (void)process_deliver_pending_signal_syscall(frame);
            return (uint64_t)result;
        }
        case NPK_SYS_TGKILL: {
            int64_t result = process_send_thread_signal((int64_t)frame->rdi, (int64_t)frame->rsi, frame->rdx);
            if (result == 0) (void)process_deliver_pending_signal_syscall(frame);
            return (uint64_t)result;
        }
        default: return neg_errno(ENOSYS);
    }
}

uint64_t syscall_dispatch_asm(uint64_t *raw) {
    if (raw == NULL) return neg_errno(EFAULT);
    thread_t *thread = scheduler_current();
    if (thread) {
        thread->saved_interrupt_frame = raw;
        thread->saved_frame_kind = 1;
    }
    syscall_frame_t frame = {0};
    frame.rax = raw[0]; frame.rbx = raw[1]; frame.rcx = raw[2]; frame.rdx = raw[3];
    frame.rsi = raw[4]; frame.rdi = raw[5]; frame.rbp = raw[6]; frame.r8 = raw[7];
    frame.r9 = raw[8]; frame.r10 = raw[9]; frame.r11 = raw[10]; frame.r12 = raw[11];
    frame.r13 = raw[12]; frame.r14 = raw[13]; frame.r15 = raw[14]; frame.rip = raw[15];
    frame.cs = raw[16]; frame.rflags = raw[17]; frame.rsp = raw[18]; frame.ss = raw[19];
    uint64_t result = syscall_dispatch(&frame);
    /* Most syscalls only change RAX, but rt_sigreturn restores the complete
     * user register/IRET frame. Copy the local ABI frame back before the
     * assembly bridge writes the final return value into raw[0]. */
    raw[0] = frame.rax; raw[1] = frame.rbx; raw[2] = frame.rcx; raw[3] = frame.rdx;
    raw[4] = frame.rsi; raw[5] = frame.rdi; raw[6] = frame.rbp; raw[7] = frame.r8;
    raw[8] = frame.r9; raw[9] = frame.r10; raw[10] = frame.r11; raw[11] = frame.r12;
    raw[12] = frame.r13; raw[13] = frame.r14; raw[14] = frame.r15; raw[15] = frame.rip;
    raw[16] = frame.cs; raw[17] = frame.rflags; raw[18] = frame.rsp; raw[19] = frame.ss;
    return result;
}

void syscall_set_current_process(process_t *process) { current_process = process; }
