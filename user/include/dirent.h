#ifndef NPK_USER_DIRENT_H
#define NPK_USER_DIRENT_H
#include <stdint.h>
#include <stddef.h>
struct dirent {
    uint64_t d_ino;
    int64_t d_off;
    uint16_t d_reclen;
    uint8_t d_type;
    char d_name[256];
};
typedef struct npk_DIR {
    int fd;
    size_t length;
    size_t offset;
    unsigned char buffer[512];
    struct dirent current;
} DIR;
DIR *opendir(const char *path);
struct dirent *readdir(DIR *directory);
int closedir(DIR *directory);
#define DT_UNKNOWN 0
#define DT_REG 8
#define DT_DIR 4
#endif
