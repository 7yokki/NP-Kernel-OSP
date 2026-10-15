#include <errno.h>
#include <npk_runtime.h>
#include <npk/user_abi.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define NPK_ALLOC_MAGIC 0x4e504b414c4c4f43ULL
#define NPK_ALLOC_PAGE 4096UL

typedef union {
    struct {
        uint64_t magic;
        size_t mapping_bytes;
        size_t requested_bytes;
    } allocation;
    long double align_long_double;
    void *align_pointer;
    uint64_t align_integer;
} allocation_header_t;

void *malloc(size_t size) {
    if (size == 0) size = 1;
    const size_t maximum = (size_t)-1;
    if (size > maximum - sizeof(allocation_header_t) - (NPK_ALLOC_PAGE - 1)) {
        errno = ENOMEM;
        return NULL;
    }
    size_t needed = sizeof(allocation_header_t) + size;
    size_t mapping_bytes = (needed + NPK_ALLOC_PAGE - 1) & ~(NPK_ALLOC_PAGE - 1);
    long result = npk_syscall6(NPK_USER_SYS_MMAP, 0, (long)mapping_bytes,
                               NPK_USER_PROT_READ | NPK_USER_PROT_WRITE,
                               NPK_USER_MAP_PRIVATE | NPK_USER_MAP_ANONYMOUS,
                               -1, 0);
    result = npk_errno_result(result);
    if (result == -1) return NULL;
    allocation_header_t *header = (allocation_header_t *)(uintptr_t)result;
    header->allocation.magic = NPK_ALLOC_MAGIC;
    header->allocation.mapping_bytes = mapping_bytes;
    header->allocation.requested_bytes = size;
    return (void *)(header + 1);
}

void free(void *pointer) {
    if (!pointer) return;
    allocation_header_t *header = (allocation_header_t *)pointer - 1;
    if (header->allocation.magic != NPK_ALLOC_MAGIC) return;
    size_t mapping_bytes = header->allocation.mapping_bytes;
    header->allocation.magic = 0;
    long result = npk_syscall6(NPK_USER_SYS_MUNMAP, (long)(uintptr_t)header,
                               (long)mapping_bytes, 0, 0, 0, 0);
    (void)npk_errno_result(result);
}

void *calloc(size_t count, size_t size) {
    if (size != 0 && count > (size_t)-1 / size) {
        errno = ENOMEM;
        return NULL;
    }
    size_t bytes = count * size;
    void *result = malloc(bytes);
    if (result) memset(result, 0, bytes);
    return result;
}

void *realloc(void *pointer, size_t size) {
    if (!pointer) return malloc(size);
    if (size == 0) {
        free(pointer);
        return NULL;
    }
    allocation_header_t *old_header = (allocation_header_t *)pointer - 1;
    if (old_header->allocation.magic != NPK_ALLOC_MAGIC) {
        errno = EINVAL;
        return NULL;
    }
    size_t old_size = old_header->allocation.requested_bytes;
    void *replacement = malloc(size);
    if (!replacement) return NULL;
    memcpy(replacement, pointer, old_size < size ? old_size : size);
    free(pointer);
    return replacement;
}

void exit(int status) {
    (void)npk_syscall6(NPK_USER_SYS_EXIT_GROUP, status, 0, 0, 0, 0, 0);
    for (;;) __asm__ volatile("hlt");
}

void _exit(int status) {
    (void)npk_syscall6(NPK_USER_SYS_EXIT, status, 0, 0, 0, 0, 0);
    for (;;) __asm__ volatile("hlt");
}

static int digit_value(unsigned char character) {
    if (character >= '0' && character <= '9') return character - '0';
    if (character >= 'a' && character <= 'z') return character - 'a' + 10;
    if (character >= 'A' && character <= 'Z') return character - 'A' + 10;
    return -1;
}

static const char *skip_space(const char *string) {
    while (*string == ' ' || *string == '\t' || *string == '\n' ||
           *string == '\r' || *string == '\v' || *string == '\f') ++string;
    return string;
}

unsigned long strtoul(const char *string, char **end, int base) {
    const char *original = string;
    if (!string) {
        if (end) *end = NULL;
        errno = EINVAL;
        return 0;
    }
    string = skip_space(string);
    int negative = 0;
    if (*string == '+' || *string == '-') { negative = *string == '-'; ++string; }
    if (base != 0 && (base < 2 || base > 36)) {
        if (end) *end = (char *)original;
        errno = EINVAL;
        return 0;
    }
    if ((base == 0 || base == 16) && string[0] == '0' &&
        (string[1] == 'x' || string[1] == 'X') &&
        digit_value((unsigned char)string[2]) >= 0 && digit_value((unsigned char)string[2]) < 16) {
        string += 2;
        base = 16;
    } else if (base == 0) {
        base = string[0] == '0' ? 8 : 10;
    }
    const char *digits = string;
    unsigned long value = 0;
    int overflow = 0;
    const unsigned long maximum = (unsigned long)-1;
    for (;;) {
        int digit = digit_value((unsigned char)*string);
        if (digit < 0 || digit >= base) break;
        if (value > (maximum - (unsigned long)digit) / (unsigned long)base) overflow = 1;
        else if (!overflow) value = value * (unsigned long)base + (unsigned long)digit;
        ++string;
    }
    if (end) *end = (char *)(string == digits ? original : string);
    if (string == digits) return 0;
    if (overflow) {
        errno = ERANGE;
        return maximum;
    }
    return negative ? 0UL - value : value;
}

long strtol(const char *string, char **end, int base) {
    const char *original = string;
    if (!string) {
        if (end) *end = NULL;
        errno = EINVAL;
        return 0;
    }
    string = skip_space(string);
    int negative = 0;
    if (*string == '+' || *string == '-') { negative = *string == '-'; ++string; }
    if (base != 0 && (base < 2 || base > 36)) {
        if (end) *end = (char *)original;
        errno = EINVAL;
        return 0;
    }
    if ((base == 0 || base == 16) && string[0] == '0' &&
        (string[1] == 'x' || string[1] == 'X') &&
        digit_value((unsigned char)string[2]) >= 0 && digit_value((unsigned char)string[2]) < 16) {
        string += 2;
        base = 16;
    } else if (base == 0) {
        base = string[0] == '0' ? 8 : 10;
    }
    const char *digits = string;
    const unsigned long positive_limit = (unsigned long)__LONG_MAX__;
    const unsigned long limit = negative ? positive_limit + 1UL : positive_limit;
    unsigned long value = 0;
    int overflow = 0;
    for (;;) {
        int digit = digit_value((unsigned char)*string);
        if (digit < 0 || digit >= base) break;
        if (value > (limit - (unsigned long)digit) / (unsigned long)base) overflow = 1;
        else if (!overflow) value = value * (unsigned long)base + (unsigned long)digit;
        ++string;
    }
    if (end) *end = (char *)(string == digits ? original : string);
    if (string == digits) return 0;
    if (overflow) {
        errno = ERANGE;
        return negative ? (-(__LONG_MAX__) - 1L) : (__LONG_MAX__);
    }
    if (negative) {
        if (value == positive_limit + 1UL) return -(__LONG_MAX__) - 1L;
        return -(long)value;
    }
    return (long)value;
}

int atoi(const char *string) { return (int)strtol(string, NULL, 10); }
int abs(int value) { return value < 0 ? -value : value; }
long labs(long value) { return value < 0 ? -value : value; }
