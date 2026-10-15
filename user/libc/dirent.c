#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <npk_runtime.h>
#include <string.h>
#include <unistd.h>

struct npk_linux_dirent64 {
    uint64_t inode;
    int64_t offset;
    uint16_t reclen;
    uint8_t type;
    char name[];
} __attribute__((packed));

static DIR directories[4];
static unsigned char directory_used[4];
DIR *opendir(const char *path) {
    unsigned slot;
    for (slot = 0; slot < 4 && directory_used[slot]; ++slot) { }
    if (slot == 4) { errno = EMFILE; return NULL; }
    int fd = open(path, O_RDONLY);
    if (fd < 0) return NULL;
    directory_used[slot] = 1;
    directories[slot].fd = fd;
    directories[slot].length = directories[slot].offset = 0;
    return &directories[slot];
}
struct dirent *readdir(DIR *directory) {
    if (!directory) { errno = EBADF; return NULL; }
    if (directory->offset >= directory->length) {
        long result = npk_errno_result(npk_syscall6(NPK_USER_SYS_GETDENTS64, directory->fd,
                                                    (long)directory->buffer, sizeof(directory->buffer), 0, 0, 0));
        if (result <= 0) { directory->length = directory->offset = 0; return NULL; }
        directory->length = (size_t)result;
        directory->offset = 0;
    }
    if (directory->length - directory->offset < sizeof(struct npk_linux_dirent64)) { errno = EIO; return NULL; }
    struct npk_linux_dirent64 *entry = (struct npk_linux_dirent64 *)(directory->buffer + directory->offset);
    if (entry->reclen < sizeof(*entry) + 1 || entry->reclen > directory->length - directory->offset) {
        errno = EIO;
        return NULL;
    }
    struct dirent *out = &directory->current;
    memset(out, 0, sizeof(*out));
    out->d_ino = entry->inode;
    out->d_off = entry->offset;
    out->d_reclen = entry->reclen;
    out->d_type = entry->type;
    size_t name_capacity = entry->reclen - sizeof(*entry);
    size_t name_length = 0;
    while (name_length < name_capacity && entry->name[name_length]) ++name_length;
    if (name_length >= sizeof(out->d_name)) name_length = sizeof(out->d_name) - 1;
    memcpy(out->d_name, entry->name, name_length);
    out->d_name[name_length] = '\0';
    directory->offset += entry->reclen;
    return out;
}
int closedir(DIR *directory) {
    if (!directory) { errno = EBADF; return -1; }
    unsigned slot = (unsigned)(directory - directories);
    if (slot >= 4 || !directory_used[slot]) { errno = EBADF; return -1; }
    int result = close(directory->fd);
    directory_used[slot] = 0;
    return result;
}
