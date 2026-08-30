#include <npk/string.h>

void *memset(void *dst, int value, size_t count) {
    uint8_t *p = (uint8_t *)dst;
    for (size_t i = 0; i < count; ++i) p[i] = (uint8_t)value;
    return dst;
}

void *memcpy(void *dst, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    for (size_t i = 0; i < count; ++i) d[i] = s[i];
    return dst;
}

void *memmove(void *dst, const void *src, size_t count) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        for (size_t i = 0; i < count; ++i) d[i] = s[i];
    } else if (d > s) {
        for (size_t i = count; i != 0; --i) d[i - 1] = s[i - 1];
    }
    return dst;
}

int memcmp(const void *a, const void *b, size_t count) {
    const uint8_t *x = (const uint8_t *)a;
    const uint8_t *y = (const uint8_t *)b;
    for (size_t i = 0; i < count; ++i) {
        if (x[i] != y[i]) return x[i] < y[i] ? -1 : 1;
    }
    return 0;
}

size_t strlen(const char *s) {
    size_t n = 0;
    while (s[n] != '\0') ++n;
    return n;
}

int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { ++a; ++b; }
    return (unsigned char)*a - (unsigned char)*b;
}

int strncmp(const char *a, const char *b, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (a[i] != b[i] || a[i] == '\0')
            return (unsigned char)a[i] - (unsigned char)b[i];
    }
    return 0;
}
