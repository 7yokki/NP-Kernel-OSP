#include <errno.h>
#include <fcntl.h>
#include <npk_runtime.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <poll.h>
#include <unistd.h>

ssize_t read(int fd, void *buffer, size_t count) {
    return (ssize_t)npk_errno_result(npk_syscall6(NPK_USER_SYS_READ, fd, (long)buffer, (long)count, 0, 0, 0));
}
ssize_t write(int fd, const void *buffer, size_t count) {
    return (ssize_t)npk_errno_result(npk_syscall6(NPK_USER_SYS_WRITE, fd, (long)buffer, (long)count, 0, 0, 0));
}
int close(int fd) {
    return (int)npk_errno_result(npk_syscall6(NPK_USER_SYS_CLOSE, fd, 0, 0, 0, 0, 0));
}
int poll(struct pollfd *fds, unsigned long count, int timeout_ms) {
    return (int)npk_errno_result(npk_syscall6(7, (long)fds, (long)count, timeout_ms, 0, 0, 0));
}
int ioctl(int fd, unsigned long request, void *argument) {
    return (int)npk_errno_result(npk_syscall6(16, fd, (long)request, (long)argument, 0, 0, 0));
}
int open(const char *path, int flags, ...) {
    return (int)npk_errno_result(npk_syscall6(NPK_USER_SYS_OPEN, (long)path, flags, 0, 0, 0, 0));
}
off_t lseek(int fd, off_t offset, int whence) {
    return (off_t)npk_errno_result(npk_syscall6(NPK_USER_SYS_LSEEK, fd, offset, whence, 0, 0, 0));
}
int chdir(const char *path) {
    return (int)npk_errno_result(npk_syscall6(NPK_USER_SYS_CHDIR, (long)path, 0, 0, 0, 0, 0));
}
char *getcwd(char *buffer, size_t size) {
    long result = npk_errno_result(npk_syscall6(NPK_USER_SYS_GETCWD, (long)buffer, (long)size, 0, 0, 0, 0));
    return result < 0 ? NULL : buffer;
}
pid_t getpid(void) { return (pid_t)npk_syscall6(NPK_USER_SYS_GETPID, 0, 0, 0, 0, 0, 0); }
pid_t fork(void) { return (pid_t)npk_errno_result(npk_syscall6(NPK_USER_SYS_FORK, 0, 0, 0, 0, 0, 0)); }
int execve(const char *path, char *const argv[], char *const envp[]) {
    return (int)npk_errno_result(npk_syscall6(NPK_USER_SYS_EXECVE, (long)path, (long)argv, (long)envp, 0, 0, 0));
}
pid_t waitpid(pid_t pid, int *status, int options) {
    return (pid_t)npk_errno_result(npk_syscall6(114, pid, (long)status, options, 0, 0, 0));
}
int stat(const char *path, struct stat *status) {
    return (int)npk_errno_result(npk_syscall6(NPK_USER_SYS_STAT, (long)path, (long)status, 0, 0, 0, 0));
}
int fstat(int fd, struct stat *status) {
    return (int)npk_errno_result(npk_syscall6(NPK_USER_SYS_FSTAT, fd, (long)status, 0, 0, 0, 0));
}
