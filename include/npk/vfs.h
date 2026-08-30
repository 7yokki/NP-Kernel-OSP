#ifndef NPK_VFS_H
#define NPK_VFS_H

#include "types.h"

typedef struct {
    const char *name;
    const uint8_t *data;
    size_t size;
} initrd_file_t;

#define NPK_VFS_POLLIN 0x001U
#define NPK_VFS_POLLPRI 0x002U
#define NPK_VFS_POLLOUT 0x004U
#define NPK_VFS_POLLERR 0x008U
#define NPK_VFS_POLLHUP 0x010U
#define NPK_VFS_POLLRDNORM NPK_VFS_POLLIN
#define NPK_VFS_POLLWRNORM NPK_VFS_POLLOUT

typedef struct {
    uint64_t data;
    uint32_t events;
    uint32_t padding;
} vfs_epoll_event_t;

typedef struct {
    uint64_t inode;
    uint32_t mode;
    uint32_t uid;
    uint32_t gid;
    uint64_t size;
    uint64_t blocks;
    uint32_t is_directory;
} vfs_stat_t;

void vfs_init(void);
#define NPK_O_WRONLY 0x0001U
#define NPK_O_RDWR 0x0002U
#define NPK_O_CREAT 0x0040U
#define NPK_O_TRUNC 0x0200U
#define NPK_O_APPEND 0x0400U

int vfs_open(const char *path);
int vfs_open_flags(const char *path, uint32_t flags, uint32_t mode);
ssize_t vfs_read(int fd, void *buffer, size_t count);
/* Offset reads do not mutate descriptor->offset and are used by lazy VM faults. */
ssize_t vfs_read_at(int fd, void *buffer, size_t count, uint64_t offset);
/* v1 accepts only immutable regular initramfs descriptors as mmap sources. */
bool vfs_file_backed(int fd);
ssize_t vfs_write(int fd, const void *buffer, size_t count);
int vfs_retain(int fd);
int vfs_close(int fd);
int vfs_sync_fd(int fd);
int vfs_sync_all(void);
ssize_t vfs_size(int fd);
int64_t vfs_seek(int fd, int64_t offset, int whence);
int vfs_stat_fd(int fd, vfs_stat_t *status);
ssize_t vfs_getdents64(int fd, void *buffer, size_t capacity);
void vfs_register_file(const char *name, const uint8_t *data, size_t size);
int vfs_pipe_create(int *read_fd, int *write_fd);
/* Anonymous shared object used by service/compositor processes. */
int vfs_shm_create(size_t size);
bool vfs_is_shared_memory(int fd);
paddr_t vfs_shm_page(int fd, size_t page_index);
size_t vfs_shm_page_count(int fd);
uint32_t vfs_poll_events(int fd, uint32_t requested);
int vfs_epoll_create(void);
int vfs_epoll_ctl(int epfd, int operation, int fd, uint32_t events, uint64_t data);
ssize_t vfs_epoll_wait(int epfd, vfs_epoll_event_t *events, size_t capacity);

#endif

