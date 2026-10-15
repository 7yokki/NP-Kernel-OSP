#ifndef NPK_USER_UNISTD_H
#define NPK_USER_UNISTD_H
#include <stddef.h>
#include <sys/types.h>
ssize_t read(int fd, void *buffer, size_t count);
ssize_t write(int fd, const void *buffer, size_t count);
off_t lseek(int fd, off_t offset, int whence);
int close(int fd);
int chdir(const char *path);
char *getcwd(char *buffer, size_t size);
pid_t getpid(void);
pid_t fork(void);
int execve(const char *path, char *const argv[], char *const envp[]);
pid_t waitpid(pid_t pid, int *status, int options);
struct pollfd;
int poll(struct pollfd *fds, unsigned long count, int timeout_ms);
int ioctl(int fd, unsigned long request, void *argument);
void _exit(int status) __attribute__((noreturn));
#endif
