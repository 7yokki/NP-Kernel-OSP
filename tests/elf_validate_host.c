#include <npk/elf.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int read_case(const char *path, unsigned char **out, size_t *size) {
    FILE *file = fopen(path, "rb");
    if (!file) return 0;
    if (fseek(file, 0, SEEK_END) != 0) { fclose(file); return 0; }
    long length = ftell(file);
    if (length < 0) { fclose(file); return 0; }
    rewind(file);
    unsigned char *buffer = (unsigned char *)malloc((size_t)length ? (size_t)length : 1U);
    if (!buffer) { fclose(file); return 0; }
    if (length != 0 && fread(buffer, 1, (size_t)length, file) != (size_t)length) {
        free(buffer);
        fclose(file);
        return 0;
    }
    fclose(file);
    *out = buffer;
    *size = (size_t)length;
    return 1;
}

int main(int argc, char **argv) {
    if (argc != 2) {
        fprintf(stderr, "usage: %s manifest.tsv\n", argv[0]);
        return 2;
    }
    FILE *manifest = fopen(argv[1], "r");
    if (!manifest) {
        perror(argv[1]);
        return 2;
    }
    char line[1024];
    unsigned total = 0;
    unsigned failed = 0;
    while (fgets(line, sizeof(line), manifest)) {
        char path[768];
        int expected = 0;
        if (sscanf(line, "%767[^\t]\t%d", path, &expected) != 2) {
            fprintf(stderr, "malformed manifest line: %s", line);
            ++failed;
            continue;
        }
        unsigned char *image = NULL;
        size_t size = 0;
        int readable = read_case(path, &image, &size);
        int actual = readable && elf64_validate(image, size);
        printf("%s: expected=%d actual=%d\n", path, expected, actual);
        if (!readable || actual != expected) ++failed;
        free(image);
        ++total;
    }
    fclose(manifest);
    printf("ELF_CORPUS total=%u failed=%u\n", total, failed);
    return failed ? 1 : 0;
}
