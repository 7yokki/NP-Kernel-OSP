#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

FILE __npk_stdin = { .fd = 0, ._flags = NPK_FILE_READABLE | NPK_FILE_STATIC };
FILE __npk_stdout = { .fd = 1, ._flags = NPK_FILE_WRITABLE | NPK_FILE_STATIC };
FILE __npk_stderr = { .fd = 2, ._flags = NPK_FILE_WRITABLE | NPK_FILE_STATIC };

static int stream_valid(FILE *stream) { return stream && stream->fd >= 0; }
static int stream_readable(FILE *stream) { return stream_valid(stream) && (stream->_flags & NPK_FILE_READABLE); }
static int stream_writable(FILE *stream) { return stream_valid(stream) && (stream->_flags & NPK_FILE_WRITABLE); }

int fileno(FILE *stream) {
    if (!stream_valid(stream)) { errno = EBADF; return -1; }
    return stream->fd;
}

FILE *fopen(const char *path, const char *mode) {
    if (!path || !mode || !mode[0]) { errno = EINVAL; return NULL; }
    int flags;
    unsigned int stream_flags;
    if (mode[0] == 'r') { flags = O_RDONLY; stream_flags = NPK_FILE_READABLE; }
    else if (mode[0] == 'w') { flags = O_WRONLY | O_CREAT | O_TRUNC; stream_flags = NPK_FILE_WRITABLE; }
    else if (mode[0] == 'a') { flags = O_WRONLY | O_CREAT | O_APPEND; stream_flags = NPK_FILE_WRITABLE; }
    else { errno = EINVAL; return NULL; }
    int plus_seen = 0;
    for (size_t i = 1; mode[i]; ++i) {
        if (mode[i] == '+') {
            if (plus_seen) { errno = EINVAL; return NULL; }
            plus_seen = 1;
        } else if (mode[i] != 'b') {
            errno = EINVAL;
            return NULL;
        }
    }
    if (plus_seen) {
        flags = (flags & ~(O_RDONLY | O_WRONLY)) | O_RDWR;
        stream_flags |= NPK_FILE_READABLE | NPK_FILE_WRITABLE;
    }
    int fd = open(path, flags, 0666);
    if (fd < 0) return NULL;
    FILE *stream = malloc(sizeof(*stream));
    if (!stream) { int saved_errno = errno; (void)close(fd); errno = saved_errno; return NULL; }
    stream->fd = fd;
    stream->_flags = stream_flags;
    stream->_pushback = 0;
    stream->_has_pushback = 0;
    return stream;
}

int fflush(FILE *stream) {
    /* Streams are currently unbuffered: every write has already reached write(2). */
    if (stream && !stream_valid(stream)) { errno = EBADF; return EOF; }
    return 0;
}

int fclose(FILE *stream) {
    if (!stream_valid(stream) || (stream->_flags & NPK_FILE_STATIC)) { errno = EBADF; return EOF; }
    int result = fflush(stream);
    int fd_result = close(stream->fd);
    stream->_flags = 0;
    stream->fd = -1;
    free(stream);
    if (result == EOF || fd_result < 0) return EOF;
    return 0;
}

int feof(FILE *stream) { return stream_valid(stream) && (stream->_flags & NPK_FILE_EOF) != 0; }
int ferror(FILE *stream) { return !stream_valid(stream) || (stream->_flags & NPK_FILE_ERROR) != 0; }
void clearerr(FILE *stream) { if (stream_valid(stream)) stream->_flags &= ~(NPK_FILE_EOF | NPK_FILE_ERROR); }

int fgetc(FILE *stream) {
    if (!stream_readable(stream)) { errno = EBADF; if (stream) stream->_flags |= NPK_FILE_ERROR; return EOF; }
    if (stream->_has_pushback) {
        stream->_has_pushback = 0;
        return stream->_pushback;
    }
    unsigned char character;
    ssize_t result = read(stream->fd, &character, 1);
    if (result == 1) return (int)character;
    if (result == 0) { stream->_flags |= NPK_FILE_EOF; return EOF; }
    stream->_flags |= NPK_FILE_ERROR;
    return EOF;
}

int getc(FILE *stream) { return fgetc(stream); }
int getchar(void) { return fgetc(stdin); }

int ungetc(int character, FILE *stream) {
    if (character == EOF || !stream_readable(stream) || stream->_has_pushback) return EOF;
    stream->_pushback = (unsigned char)character;
    stream->_has_pushback = 1;
    stream->_flags &= ~NPK_FILE_EOF;
    return stream->_pushback;
}

int fputc(int character, FILE *stream) {
    if (!stream_writable(stream)) { errno = EBADF; if (stream) stream->_flags |= NPK_FILE_ERROR; return EOF; }
    unsigned char value = (unsigned char)character;
    ssize_t result = write(stream->fd, &value, 1);
    if (result != 1) { stream->_flags |= NPK_FILE_ERROR; return EOF; }
    return value;
}

int putc(int character, FILE *stream) { return fputc(character, stream); }
int putchar(int character) { return fputc(character, stdout); }

size_t fread(void *buffer, size_t size, size_t count, FILE *stream) {
    if (size == 0 || count == 0) return 0;
    if (!buffer || !stream_readable(stream)) {
        errno = !buffer ? EFAULT : EBADF;
        if (stream) stream->_flags |= NPK_FILE_ERROR;
        return 0;
    }
    if (count > (size_t)-1 / size) { errno = EOVERFLOW; stream->_flags |= NPK_FILE_ERROR; return 0; }
    size_t wanted = size * count;
    size_t received = 0;
    unsigned char *out = buffer;
    if (stream->_has_pushback && wanted) {
        out[received++] = stream->_pushback;
        stream->_has_pushback = 0;
    }
    while (received < wanted) {
        ssize_t result = read(stream->fd, out + received, wanted - received);
        if (result < 0) { stream->_flags |= NPK_FILE_ERROR; break; }
        if (result == 0) { stream->_flags |= NPK_FILE_EOF; break; }
        received += (size_t)result;
    }
    return received / size;
}

size_t fwrite(const void *buffer, size_t size, size_t count, FILE *stream) {
    if (size == 0 || count == 0) return 0;
    if (!buffer || !stream_writable(stream)) {
        errno = !buffer ? EFAULT : EBADF;
        if (stream) stream->_flags |= NPK_FILE_ERROR;
        return 0;
    }
    if (count > (size_t)-1 / size) { errno = EOVERFLOW; stream->_flags |= NPK_FILE_ERROR; return 0; }
    size_t wanted = size * count;
    size_t written = 0;
    const unsigned char *in = buffer;
    while (written < wanted) {
        ssize_t result = write(stream->fd, in + written, wanted - written);
        if (result < 0) { stream->_flags |= NPK_FILE_ERROR; break; }
        if (result == 0) { errno = EIO; stream->_flags |= NPK_FILE_ERROR; break; }
        written += (size_t)result;
    }
    return written / size;
}

char *fgets(char *buffer, int size, FILE *stream) {
    if (!buffer || size <= 0 || !stream_readable(stream)) {
        errno = !buffer || size <= 0 ? EINVAL : EBADF;
        if (stream) stream->_flags |= NPK_FILE_ERROR;
        return NULL;
    }
    int used = 0;
    while (used + 1 < size) {
        int character = fgetc(stream);
        if (character == EOF) break;
        buffer[used++] = (char)character;
        if (character == '\n') break;
    }
    if (used == 0) return NULL;
    buffer[used] = '\0';
    return buffer;
}

static int write_all(FILE *stream, const char *bytes, size_t length) {
    size_t written = 0;
    while (written < length) {
        ssize_t result = write(stream->fd, bytes + written, length - written);
        if (result < 0) { stream->_flags |= NPK_FILE_ERROR; return -1; }
        if (result == 0) { errno = EIO; stream->_flags |= NPK_FILE_ERROR; return -1; }
        written += (size_t)result;
    }
    return 0;
}

int fputs(const char *string, FILE *stream) {
    if (!string || !stream_writable(stream)) {
        errno = !string ? EFAULT : EBADF;
        if (stream) stream->_flags |= NPK_FILE_ERROR;
        return EOF;
    }
    return write_all(stream, string, strlen(string)) < 0 ? EOF : 0;
}

int puts(const char *string) {
    if (fputs(string, stdout) == EOF || putchar('\n') == EOF) return EOF;
    return 0;
}

int fseek(FILE *stream, long offset, int whence) {
    if (!stream_valid(stream) || whence < SEEK_SET || whence > SEEK_END) { errno = EINVAL; return -1; }
    if (whence == SEEK_CUR && stream->_has_pushback) {
        if (offset == -(__LONG_MAX__) - 1L) { errno = EINVAL; return -1; }
        --offset;
    }
    if (lseek(stream->fd, (off_t)offset, whence) < 0) { stream->_flags |= NPK_FILE_ERROR; return -1; }
    stream->_has_pushback = 0;
    stream->_flags &= ~NPK_FILE_EOF;
    return 0;
}

long ftell(FILE *stream) {
    if (!stream_valid(stream)) { errno = EBADF; return -1L; }
    off_t position = lseek(stream->fd, 0, SEEK_CUR);
    if (position < 0) { stream->_flags |= NPK_FILE_ERROR; return -1L; }
    if (stream->_has_pushback && position > 0) --position;
    return (long)position;
}

void rewind(FILE *stream) {
    if (stream_valid(stream)) {
        if (fseek(stream, 0, SEEK_SET) == 0) clearerr(stream);
    }
}

static void append_char(char *buffer, size_t capacity, size_t *length, char character) {
    if (capacity != 0 && *length + 1 < capacity) buffer[*length] = character;
    ++*length;
}

static void append_unsigned(char *buffer, size_t capacity, size_t *length,
                            uint64_t value, unsigned base, int upper) {
    char digits[32];
    const char *alphabet = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    size_t count = 0;
    do { digits[count++] = alphabet[value % base]; value /= base; } while (value != 0 && count < sizeof(digits));
    while (count != 0) append_char(buffer, capacity, length, digits[--count]);
}

int vsnprintf(char *buffer, size_t capacity, const char *format, va_list arguments) {
    if (!format || (!buffer && capacity != 0)) { errno = EINVAL; return -1; }
    size_t length = 0;
    for (size_t i = 0; format[i]; ++i) {
        if (format[i] != '%') { append_char(buffer, capacity, &length, format[i]); continue; }
        ++i;
        if (format[i] == '%') { append_char(buffer, capacity, &length, '%'); continue; }
        int long_count = 0;
        while (format[i] == 'l' || format[i] == 'z') { ++long_count; ++i; }
        char specifier = format[i];
        if (specifier == 's') {
            const char *value = va_arg(arguments, const char *);
            if (!value) value = "(null)";
            while (*value) append_char(buffer, capacity, &length, *value++);
        } else if (specifier == 'c') {
            append_char(buffer, capacity, &length, (char)va_arg(arguments, int));
        } else if (specifier == 'd' || specifier == 'i') {
            int64_t value = long_count >= 2 ? va_arg(arguments, long long) :
                            long_count ? va_arg(arguments, long) : va_arg(arguments, int);
            uint64_t magnitude;
            if (value < 0) { append_char(buffer, capacity, &length, '-'); magnitude = (uint64_t)(-(value + 1)) + 1; }
            else magnitude = (uint64_t)value;
            append_unsigned(buffer, capacity, &length, magnitude, 10, 0);
        } else if (specifier == 'u' || specifier == 'x' || specifier == 'X' || specifier == 'p') {
            uint64_t value = specifier == 'p' ? (uint64_t)(uintptr_t)va_arg(arguments, void *) :
                             long_count >= 2 ? va_arg(arguments, unsigned long long) :
                             long_count ? va_arg(arguments, unsigned long) : va_arg(arguments, unsigned int);
            if (specifier == 'p') { append_char(buffer, capacity, &length, '0'); append_char(buffer, capacity, &length, 'x'); }
            append_unsigned(buffer, capacity, &length, value, specifier == 'u' ? 10U : 16U, specifier == 'X');
        } else {
            append_char(buffer, capacity, &length, '%');
            if (specifier) append_char(buffer, capacity, &length, specifier);
        }
    }
    if (capacity != 0) buffer[length < capacity ? length : capacity - 1] = '\0';
    return length > 0x7fffffffU ? -1 : (int)length;
}

static int vformat_to_file(FILE *stream, const char *format, va_list arguments) {
    if (!stream_writable(stream) || !format) { errno = EINVAL; return -1; }
    va_list measure_args;
    va_copy(measure_args, arguments);
    int length = vsnprintf(NULL, 0, format, measure_args);
    va_end(measure_args);
    if (length < 0) return -1;
    if ((size_t)length > 1024U * 1024U) { errno = EOVERFLOW; return -1; }
    char stack_buffer[256];
    char *buffer = (size_t)length < sizeof(stack_buffer) ? stack_buffer : malloc((size_t)length + 1);
    if (!buffer) return -1;
    va_list format_args;
    va_copy(format_args, arguments);
    int rendered = vsnprintf(buffer, (size_t)length + 1, format, format_args);
    va_end(format_args);
    int result = rendered == length && write_all(stream, buffer, (size_t)length) == 0 ? length : -1;
    if (buffer != stack_buffer) free(buffer);
    return result;
}

int vfprintf(FILE *stream, const char *format, va_list arguments) { return vformat_to_file(stream, format, arguments); }
int vprintf(const char *format, va_list arguments) { return vfprintf(stdout, format, arguments); }

int fprintf(FILE *stream, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vfprintf(stream, format, arguments);
    va_end(arguments);
    return result;
}

int printf(const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vprintf(format, arguments);
    va_end(arguments);
    return result;
}

int snprintf(char *buffer, size_t capacity, const char *format, ...) {
    va_list arguments;
    va_start(arguments, format);
    int result = vsnprintf(buffer, capacity, format, arguments);
    va_end(arguments);
    return result;
}
