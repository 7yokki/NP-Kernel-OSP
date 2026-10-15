#include <errno.h>
#include <fcntl.h>
#include <npk_runtime.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>

#define MOCK_FILE_FD 3
#define MOCK_FILE_CAPACITY 4096
#define MOCK_MAP_CAPACITY (64U * 4096U)

static _Alignas(4096) unsigned char mapped_memory[MOCK_MAP_CAPACITY];
static size_t mapped_offset;
static unsigned char mock_file[MOCK_FILE_CAPACITY];
static size_t mock_file_length;
static size_t mock_file_offset;
int npk_errno;

long npk_errno_result(long result) {
    if (result < 0 && result >= -4095) { npk_errno = (int)-result; return -1; }
    return result;
}

long npk_syscall6(long number, long a1, long a2, long a3, long a4, long a5, long a6) {
    (void)a1; (void)a3; (void)a4; (void)a5; (void)a6;
    if (number == NPK_USER_SYS_MMAP) {
        size_t bytes = (size_t)a2;
        if (bytes == 0 || bytes > MOCK_MAP_CAPACITY - mapped_offset) return -ENOMEM;
        void *result = mapped_memory + mapped_offset;
        mapped_offset += bytes;
        return (long)(uintptr_t)result;
    }
    if (number == NPK_USER_SYS_MUNMAP) return 0;
    return -ENOSYS;
}

int npk_test_open(const char *path, int flags, ...) {
    if (!path) { npk_errno = EFAULT; return -1; }
    if (flags & O_TRUNC) mock_file_length = 0;
    mock_file_offset = (flags & O_APPEND) ? mock_file_length : 0;
    return MOCK_FILE_FD;
}

ssize_t npk_test_read(int fd, void *buffer, size_t count) {
    if (fd != MOCK_FILE_FD || !buffer) { npk_errno = EBADF; return -1; }
    if (mock_file_offset >= mock_file_length) return 0;
    size_t available = mock_file_length - mock_file_offset;
    size_t bytes = count < available ? count : available;
    for (size_t i = 0; i < bytes; ++i) ((unsigned char *)buffer)[i] = mock_file[mock_file_offset + i];
    mock_file_offset += bytes;
    return (ssize_t)bytes;
}

ssize_t npk_test_write(int fd, const void *buffer, size_t count) {
    if ((fd != MOCK_FILE_FD && fd != 1 && fd != 2) || !buffer) { npk_errno = EBADF; return -1; }
    if (fd != MOCK_FILE_FD) return (ssize_t)count;
    if (mock_file_offset > MOCK_FILE_CAPACITY || count > MOCK_FILE_CAPACITY - mock_file_offset) {
        npk_errno = ENOMEM;
        return -1;
    }
    for (size_t i = 0; i < count; ++i) mock_file[mock_file_offset + i] = ((const unsigned char *)buffer)[i];
    mock_file_offset += count;
    if (mock_file_length < mock_file_offset) mock_file_length = mock_file_offset;
    return (ssize_t)count;
}

int npk_test_close(int fd) { return fd == MOCK_FILE_FD ? 0 : -1; }

off_t npk_test_lseek(int fd, off_t offset, int whence) {
    if (fd != MOCK_FILE_FD) { npk_errno = EBADF; return -1; }
    off_t base;
    if (whence == SEEK_SET) base = 0;
    else if (whence == SEEK_CUR) base = (off_t)mock_file_offset;
    else if (whence == SEEK_END) base = (off_t)mock_file_length;
    else { npk_errno = EINVAL; return -1; }
    if (offset < -base || base + offset > MOCK_FILE_CAPACITY) {
        npk_errno = EINVAL;
        return -1;
    }
    mock_file_offset = (size_t)(base + offset);
    return (off_t)mock_file_offset;
}

static int test_allocator(void) {
    unsigned char *bytes = malloc(12);
    if (!bytes || ((uintptr_t)bytes & 15U) != 0) return 1;
    for (size_t i = 0; i < 12; ++i) bytes[i] = (unsigned char)(i + 1);
    unsigned char *larger = realloc(bytes, 64);
    if (!larger) return 2;
    for (size_t i = 0; i < 12; ++i) if (larger[i] != (unsigned char)(i + 1)) return 3;
    free(larger);
    unsigned char *zero = calloc(17, 1);
    if (!zero) return 4;
    for (size_t i = 0; i < 17; ++i) if (zero[i] != 0) return 5;
    free(zero);
    void *empty = malloc(0);
    if (!empty) return 6;
    free(empty);
    return 0;
}

static int test_string(void) {
    char buffer[32] = "npk";
    if (strnlen(buffer, 2) != 2) return 1;
    if (strcmp(strcat(buffer, "ernel"), "npkernel") != 0) return 2;
    if (strcmp(strncat(buffer, "-shell", 4), "npkernel-she") != 0) return 3;
    if (strcmp(strstr(buffer, "kernel"), "kernel-she") != 0) return 4;
    if (strspn("abc123", "abc") != 3 || strcspn("abc123", "0123456789") != 3) return 5;
    if (memchr(buffer, '-', strlen(buffer)) != buffer + 8) return 6;
    char *copy = strdup("freestanding");
    if (!copy || strcmp(copy, "freestanding") != 0) return 7;
    free(copy);
    copy = strndup("abcdef", 3);
    if (!copy || strcmp(copy, "abc") != 0) return 8;
    free(copy);
    return 0;
}

static int test_integer_conversion(void) {
    char *end;
    const long long_max = (long)((unsigned long)-1 >> 1);
    const long long_min = -long_max - 1;
    if (strtol("  -0x2aZ", &end, 0) != -42 || *end != 'Z') return 1;
    if (strtol("-9223372036854775808", &end, 10) != long_min || *end != '\0') return 2;
    npk_errno = 0;
    if (strtol("9223372036854775808", &end, 10) != long_max || npk_errno != ERANGE) return 3;
    if (strtoul("0x2a!", &end, 0) != 42 || *end != '!') return 4;
    if (strtoul("-1", &end, 10) != (unsigned long)-1 || *end != '\0') return 5;
    const char invalid[] = "no digits";
    if (strtol(invalid, &end, 10) != 0 || end != invalid) return 6;
    return 0;
}

static int test_stdio(void) {
    FILE *stream = fopen("/libc-test", "w+");
    if (!stream) return 1;
    const char first[] = "abc";
    if (fwrite(first, 1, 3, stream) != 3) return 2;
    if (fprintf(stream, "-%s-%zu", "ok", (size_t)12) != 6) return 3;
    if (ftell(stream) != 9) return 4;
    if (fflush(stream) != 0 || fseek(stream, 0, SEEK_SET) != 0) return 5;
    char content[16] = {0};
    if (fread(content, 1, 9, stream) != 9) return 6;
    if (strcmp(content, "abc-ok-12") != 0) return 7;
    if (feof(stream)) return 8;
    if (fgetc(stream) != EOF || !feof(stream)) return 9;
    clearerr(stream);
    if (ferror(stream) || feof(stream)) return 10;
    if (fseek(stream, 0, SEEK_SET) != 0 || fgetc(stream) != 'a' || ungetc('X', stream) != 'X') return 11;
    if (fseek(stream, 0, SEEK_CUR) != 0 || fgetc(stream) != 'a') return 12;
    rewind(stream);
    if (fgets(content, sizeof(content), stream) != content || strcmp(content, "abc-ok-12") != 0) return 13;
    if (fclose(stream) != 0) return 14;
    stream = fopen("/libc-test", "a");
    if (!stream) return 15;
    if (fputs("!", stream) != 0 || fclose(stream) != 0) return 16;
    stream = fopen("/libc-test", "r");
    if (!stream) return 17;
    if (fread(content, 1, sizeof(content), stream) != 10) return 18;
    if (strcmp(content, "abc-ok-12!") != 0) return 19;
    if (fclose(stream) != 0) return 20;
    if (fopen("/libc-test", "bad") != NULL || npk_errno != EINVAL) return 21;
    return 0;
}

static int test_formatting(void) {
    char buffer[32];
    int length = snprintf(buffer, sizeof(buffer), "%s:%d:%zu", "ok", -7, (size_t)42);
    return length == 8 && strcmp(buffer, "ok:-7:42") == 0 ? 0 : 1;
}

int main(void) {
    int result = test_allocator();
    if (result) return 10 + result;
    result = test_string();
    if (result) return 30 + result;
    result = test_integer_conversion();
    if (result) return 45 + result;
    result = test_stdio();
    if (result) return 60 + result;
    result = test_formatting();
    if (result) return 90 + result;
    return 0;
}
