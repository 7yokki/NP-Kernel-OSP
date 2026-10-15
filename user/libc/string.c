#include <errno.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

void *memcpy(void *destination, const void *source, size_t count) {
    unsigned char *out = destination;
    const unsigned char *in = source;
    for (size_t i = 0; i < count; ++i) out[i] = in[i];
    return destination;
}

void *memmove(void *destination, const void *source, size_t count) {
    unsigned char *out = destination;
    const unsigned char *in = source;
    uintptr_t out_address = (uintptr_t)out;
    uintptr_t in_address = (uintptr_t)in;
    if (out_address <= in_address || out_address - in_address >= count) {
        for (size_t i = 0; i < count; ++i) out[i] = in[i];
    } else {
        for (size_t i = count; i != 0; --i) out[i - 1] = in[i - 1];
    }
    return destination;
}

void *memset(void *destination, int value, size_t count) {
    unsigned char *out = destination;
    for (size_t i = 0; i < count; ++i) out[i] = (unsigned char)value;
    return destination;
}

int memcmp(const void *left, const void *right, size_t count) {
    const unsigned char *a = left, *b = right;
    for (size_t i = 0; i < count; ++i) if (a[i] != b[i]) return (int)a[i] - (int)b[i];
    return 0;
}

void *memchr(const void *memory, int value, size_t count) {
    const unsigned char *bytes = memory;
    unsigned char wanted = (unsigned char)value;
    for (size_t i = 0; i < count; ++i) if (bytes[i] == wanted) return (void *)(bytes + i);
    return NULL;
}

size_t strlen(const char *string) { size_t n = 0; while (string[n]) ++n; return n; }
size_t strnlen(const char *string, size_t maximum) {
    size_t n = 0;
    while (n < maximum && string[n]) ++n;
    return n;
}

int strcmp(const char *left, const char *right) {
    while (*left && *left == *right) { ++left; ++right; }
    return (unsigned char)*left - (unsigned char)*right;
}

int strncmp(const char *left, const char *right, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        unsigned char a = (unsigned char)left[i], b = (unsigned char)right[i];
        if (a != b || a == 0) return (int)a - (int)b;
    }
    return 0;
}

char *strcpy(char *destination, const char *source) {
    char *result = destination;
    while ((*destination++ = *source++) != '\0') { }
    return result;
}

char *strncpy(char *destination, const char *source, size_t count) {
    size_t i = 0;
    while (i < count && source[i]) { destination[i] = source[i]; ++i; }
    while (i < count) destination[i++] = '\0';
    return destination;
}

char *strcat(char *destination, const char *source) {
    char *result = destination + strlen(destination);
    while ((*result++ = *source++) != '\0') { }
    return destination;
}

char *strncat(char *destination, const char *source, size_t count) {
    char *result = destination + strlen(destination);
    size_t i = 0;
    while (i < count && source[i]) { result[i] = source[i]; ++i; }
    result[i] = '\0';
    return destination;
}

char *strchr(const char *string, int character) {
    char wanted = (char)character;
    for (;;) { if (*string == wanted) return (char *)string; if (!*string) return NULL; ++string; }
}

char *strrchr(const char *string, int character) {
    char wanted = (char)character;
    const char *last = NULL;
    do { if (*string == wanted) last = string; } while (*string++);
    return (char *)last;
}

char *strstr(const char *haystack, const char *needle) {
    if (!*needle) return (char *)haystack;
    size_t needle_length = strlen(needle);
    for (; *haystack; ++haystack)
        if (*haystack == *needle && strncmp(haystack, needle, needle_length) == 0) return (char *)haystack;
    return NULL;
}

static int contains_character(const char *set, unsigned char character) {
    for (; *set; ++set) if ((unsigned char)*set == character) return 1;
    return 0;
}

size_t strspn(const char *string, const char *accept) {
    size_t length = 0;
    while (string[length] && contains_character(accept, (unsigned char)string[length])) ++length;
    return length;
}

size_t strcspn(const char *string, const char *reject) {
    size_t length = 0;
    while (string[length] && !contains_character(reject, (unsigned char)string[length])) ++length;
    return length;
}

char *strpbrk(const char *string, const char *accept) {
    for (; *string; ++string) if (contains_character(accept, (unsigned char)*string)) return (char *)string;
    return NULL;
}

char *strtok(char *string, const char *delimiters) {
    static char *next;
    if (string) next = string;
    if (!next) return NULL;
    while (*next && contains_character(delimiters, (unsigned char)*next)) ++next;
    if (!*next) { next = NULL; return NULL; }
    char *token = next;
    while (*next && !contains_character(delimiters, (unsigned char)*next)) ++next;
    if (*next) *next++ = '\0'; else next = NULL;
    return token;
}

char *strndup(const char *string, size_t maximum) {
    size_t length = strnlen(string, maximum);
    if (length == (size_t)-1) { errno = ENOMEM; return NULL; }
    char *copy = malloc(length + 1);
    if (!copy) return NULL;
    memcpy(copy, string, length);
    copy[length] = '\0';
    return copy;
}

char *strdup(const char *string) { return strndup(string, (size_t)-1); }
