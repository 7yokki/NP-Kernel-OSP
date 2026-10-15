#ifndef NPK_USER_STDIO_H
#define NPK_USER_STDIO_H

#include <stddef.h>
#include <stdarg.h>
#include <sys/types.h>

#define EOF (-1)
#define BUFSIZ 1024
#define SEEK_SET 0
#define SEEK_CUR 1
#define SEEK_END 2

/* This mini-libc currently performs stdio I/O synchronously without buffering. */
typedef struct npk_FILE {
    int fd;
    unsigned int _flags;
    unsigned char _pushback;
    unsigned char _has_pushback;
} FILE;

#define NPK_FILE_READABLE  0x01U
#define NPK_FILE_WRITABLE  0x02U
#define NPK_FILE_EOF       0x04U
#define NPK_FILE_ERROR     0x08U
#define NPK_FILE_STATIC    0x10U

extern FILE __npk_stdin;
extern FILE __npk_stdout;
extern FILE __npk_stderr;
#define stdin (&__npk_stdin)
#define stdout (&__npk_stdout)
#define stderr (&__npk_stderr)

FILE *fopen(const char *path, const char *mode);
int fclose(FILE *stream);
int fflush(FILE *stream);
int fileno(FILE *stream);
int feof(FILE *stream);
int ferror(FILE *stream);
void clearerr(FILE *stream);
int fgetc(FILE *stream);
int getc(FILE *stream);
int getchar(void);
int ungetc(int character, FILE *stream);
int fputc(int character, FILE *stream);
int putc(int character, FILE *stream);
int putchar(int character);
size_t fread(void *buffer, size_t size, size_t count, FILE *stream);
size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream);
char *fgets(char *buffer, int size, FILE *stream);
int fputs(const char *string, FILE *stream);
int puts(const char *string);
int fseek(FILE *stream, long offset, int whence);
long ftell(FILE *stream);
void rewind(FILE *stream);
int printf(const char *format, ...);
int fprintf(FILE *stream, const char *format, ...);
int vprintf(const char *format, va_list arguments);
int vfprintf(FILE *stream, const char *format, va_list arguments);
int snprintf(char *buffer, size_t capacity, const char *format, ...);
int vsnprintf(char *buffer, size_t capacity, const char *format, va_list arguments);

#endif
