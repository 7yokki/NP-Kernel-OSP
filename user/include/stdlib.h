#ifndef NPK_USER_STDLIB_H
#define NPK_USER_STDLIB_H

#include <stddef.h>

#define EXIT_SUCCESS 0
#define EXIT_FAILURE 1

void *malloc(size_t size);
void *calloc(size_t count, size_t size);
void *realloc(void *pointer, size_t size);
void free(void *pointer);
void exit(int status) __attribute__((noreturn));
void _exit(int status) __attribute__((noreturn));
int atoi(const char *string);
long strtol(const char *string, char **end, int base);
unsigned long strtoul(const char *string, char **end, int base);
int abs(int value);
long labs(long value);

#endif
