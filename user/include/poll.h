#ifndef NPK_USER_POLL_H
#define NPK_USER_POLL_H
#include <sys/types.h>

struct pollfd {
    int fd;
    short events;
    short revents;
};

#define POLLIN  0x001
#define POLLPRI 0x002
#define POLLOUT 0x004
#define POLLERR 0x008
#define POLLHUP 0x010
#define POLLNVAL 0x020

int poll(struct pollfd *fds, unsigned long count, int timeout_ms);
#endif
