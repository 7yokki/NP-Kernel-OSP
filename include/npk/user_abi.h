#ifndef NPK_USER_ABI_H
#define NPK_USER_ABI_H

/*
 * NPKernel user ABI v1.
 * This header describes the prototype contract implemented by the kernel. It
 * is not a libc, startup object, allocator, pthread implementation, or GUI.
 */
#include "types.h"

#define NPK_USER_ABI_VERSION 1U
#define NPK_USER_SYSCALL_MAX_ARGS 6U
#define NPK_USER_SYSCALL_ERROR_MIN (-4095LL)
#define NPK_USER_SYSCALL_ERROR_MAX (-1LL)

/* x86_64 syscall instruction ABI: rax=number, rdi/rsi/rdx/r10/r8/r9=args.
 * rcx and r11 are clobbered by the CPU syscall/sysret transition. */
#define NPK_USER_SYS_READ 0
#define NPK_USER_SYS_WRITE 1
#define NPK_USER_SYS_OPEN 2
#define NPK_USER_SYS_CLOSE 3
#define NPK_USER_SYS_STAT 4
#define NPK_USER_SYS_FSTAT 5
#define NPK_USER_SYS_LSEEK 8
#define NPK_USER_SYS_MMAP 9
#define NPK_USER_SYS_MPROTECT 10
#define NPK_USER_SYS_MUNMAP 11
#define NPK_USER_SYS_BRK 12
#define NPK_USER_SYS_RT_SIGACTION 13
#define NPK_USER_SYS_RT_SIGPROCMASK 14
#define NPK_USER_SYS_RT_SIGRETURN 15
#define NPK_USER_SYS_IOCTL 16
#define NPK_USER_SYS_READV 19
#define NPK_USER_SYS_WRITEV 20
#define NPK_USER_SYS_PIPE 22
#define NPK_USER_SYS_SCHED_YIELD 24
#define NPK_USER_SYS_NANOSLEEP 35
#define NPK_USER_SYS_DUP 32
#define NPK_USER_SYS_DUP2 33
#define NPK_USER_SYS_FCNTL 72
#define NPK_USER_SYS_FSYNC 74
#define NPK_USER_SYS_FDATASYNC 75
#define NPK_USER_SYS_GETCWD 79
#define NPK_USER_SYS_READLINK 89
#define NPK_USER_SYS_RT_SIGQUEUEINFO 129
#define NPK_USER_SYS_GETPID 39
#define NPK_USER_SYS_UNAME 63
#define NPK_USER_SYS_FORK 57
#define NPK_USER_SYS_CLONE 56
#define NPK_USER_SYS_EXECVE 59
#define NPK_USER_SYS_EXIT 60
#define NPK_USER_SYS_WAIT4 61
#define NPK_USER_SYS_KILL 62
#define NPK_USER_SYS_TGKILL 234
#define NPK_USER_SYS_ARCH_PRCTL 158
#define NPK_USER_SYS_GETTID 186
#define NPK_USER_SYS_CLOCK_GETTIME 228
#define NPK_USER_SYS_EXIT_GROUP 231
#define NPK_USER_SYS_GETDENTS64 217
#define NPK_USER_SYS_FUTEX 202
#define NPK_USER_SYS_EPOLL_WAIT 232
#define NPK_USER_SYS_EPOLL_CTL 233
#define NPK_USER_SYS_PPOLL 271
#define NPK_USER_SYS_EPOLL_CREATE1 291
#define NPK_USER_SYS_RT_TGSIGQUEUEINFO 297
#define NPK_USER_SYS_SET_TID_ADDRESS 218
#define NPK_USER_SYS_SYNC 162
#define NPK_USER_SYS_OPENAT 257
#define NPK_USER_SYS_NPK_SHM_CREATE 0x400
#define NPK_USER_SYS_NPK_FD_SEND 0x401
#define NPK_USER_SYS_NPK_FD_RECV 0x402
#define NPK_USER_SYS_NPK_DISPLAY_INFO 0x403
#define NPK_USER_SYS_NPK_DISPLAY_CLAIM 0x404
#define NPK_USER_SYS_NPK_DISPLAY_MAP 0x405
#define NPK_USER_SYS_NPK_DISPLAY_RELEASE 0x406
#define NPK_USER_SYS_NPK_INPUT_READ 0x407

/* mmap/mprotect protection and mapping flags. MAP_HEAP is kernel-private and
 * must not be passed by user programs. A file mapping currently accepts only
 * immutable initramfs regular files and MAP_PRIVATE. */
#define NPK_USER_PROT_NONE 0ULL
#define NPK_USER_PROT_READ 1ULL
#define NPK_USER_PROT_WRITE 2ULL
#define NPK_USER_PROT_EXEC 4ULL
#define NPK_USER_MAP_SHARED 1ULL
#define NPK_USER_MAP_PRIVATE 2ULL
#define NPK_USER_MAP_FIXED 16ULL
#define NPK_USER_MAP_ANONYMOUS 32ULL
#define NPK_USER_SHM_MAX_BYTES (256ULL * 4096ULL)
#define NPK_USER_MAP_FAILED ((uint64_t)-1)

/* Process/thread creation subset. Full pthread, robust-list, PI, namespace,
 * signal-stack, and clone3 semantics are not part of ABI v1. */
#define NPK_USER_CLONE_VM 0x0000000000000100ULL
#define NPK_USER_CLONE_FS 0x0000000000000200ULL
#define NPK_USER_CLONE_FILES 0x0000000000000400ULL
#define NPK_USER_CLONE_SIGHAND 0x0000000000000800ULL
#define NPK_USER_CLONE_THREAD 0x0000000000010000ULL
#define NPK_USER_CLONE_SETTLS 0x0000000000080000ULL
#define NPK_USER_CLONE_PARENT_SETTID 0x0000000010000000ULL
#define NPK_USER_CLONE_CHILD_CLEARTID 0x0000000020000000ULL
#define NPK_USER_CLONE_CHILD_SETTID 0x0000000100000000ULL
#define NPK_USER_CLONE_SYSVSEM 0x0000000000040000ULL

#define NPK_USER_FUTEX_WAIT 0U
#define NPK_USER_FUTEX_WAKE 1U
#define NPK_USER_FUTEX_PRIVATE_FLAG 128U
#define NPK_USER_FD_TRANSFER_MAX 8U

#define NPK_USER_ARCH_SET_GS 0x1001U
#define NPK_USER_ARCH_SET_FS 0x1002U
#define NPK_USER_ARCH_GET_FS 0x1003U
#define NPK_USER_ARCH_GET_GS 0x1004U

#define NPK_USER_AT_NULL 0ULL
#define NPK_USER_AT_PHDR 3ULL
#define NPK_USER_AT_PHENT 4ULL
#define NPK_USER_AT_PHNUM 5ULL
#define NPK_USER_AT_PAGESZ 6ULL
#define NPK_USER_AT_BASE 7ULL
#define NPK_USER_AT_ENTRY 9ULL

typedef struct {
    uint64_t type;
    uint64_t value;
} npk_user_auxv_t;

typedef struct {
    int64_t seconds;
    int64_t nanoseconds;
} npk_user_timespec_t;

typedef struct {
    uint32_t c_iflag;
    uint32_t c_oflag;
    uint32_t c_cflag;
    uint32_t c_lflag;
    uint8_t c_line;
    uint8_t c_cc[32];
    uint32_t c_ispeed;
    uint32_t c_ospeed;
} npk_user_termios_t;

typedef struct {
    uint16_t rows;
    uint16_t columns;
    uint16_t x_pixels;
    uint16_t y_pixels;
} npk_user_winsize_t;

typedef struct {
    uint64_t physical_base;
    uint64_t page_offset;
    uint64_t mapping_size;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
} npk_user_display_info_t;

typedef struct {
    uint64_t timestamp;
    uint16_t type;
    uint16_t code;
    int32_t value;
    uint32_t modifiers;
    uint32_t reserved;
} npk_user_input_event_t;

#define NPK_USER_INPUT_EVENT_SYN 0U
#define NPK_USER_INPUT_EVENT_KEY 1U
#define NPK_USER_INPUT_EVENT_REL 2U
#define NPK_USER_INPUT_EVENT_ABS 3U
#define NPK_USER_INPUT_KEY_PRESS 1
#define NPK_USER_INPUT_KEY_RELEASE 0
#define NPK_USER_INPUT_MOD_SHIFT (1U << 0)
#define NPK_USER_INPUT_MOD_CAPSLOCK (1U << 1)
#define NPK_USER_INPUT_MOD_EXTENDED (1U << 2)

#define NPK_USER_IOCTL_TCGETS 0x5401ULL
#define NPK_USER_IOCTL_TCSETS 0x5402ULL
#define NPK_USER_IOCTL_TCSETSW 0x5403ULL
#define NPK_USER_IOCTL_TCSETSF 0x5404ULL
#define NPK_USER_IOCTL_TIOCGWINSZ 0x5413ULL
#define NPK_USER_IOCTL_TIOCSWINSZ 0x5414ULL
#define NPK_USER_IOCTL_FIONREAD 0x541bULL

#endif
