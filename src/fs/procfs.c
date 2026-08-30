#include <npk/block.h>
#include <npk/memory.h>
#include <npk/process.h>
#include <npk/procfs.h>
#include <npk/string.h>
#include <npk/timer.h>

static bool path_eq(const char *a, const char *b) { return a && b && strcmp(a, b) == 0; }

bool procfs_is_path(const char *path) {
    return path && (strncmp(path, "/proc/", 6) == 0 || strcmp(path, "/proc") == 0);
}

static bool append_text(char *buffer, size_t capacity, size_t *length, const char *text) {
    size_t text_length = strlen(text);
    if (*length > capacity || text_length > capacity - *length) return false;
    memcpy(buffer + *length, text, text_length);
    *length += text_length;
    return true;
}

static bool append_u64(char *buffer, size_t capacity, size_t *length, uint64_t value) {
    char digits[21];
    size_t count = 0;
    if (value == 0) digits[count++] = '0';
    while (value != 0 && count < sizeof(digits)) { digits[count++] = (char)('0' + value % 10); value /= 10; }
    if (*length > capacity || count > capacity - *length) return false;
    while (count > 0) buffer[(*length)++] = digits[--count];
    return true;
}

ssize_t procfs_snapshot(const char *path, char *buffer, size_t capacity) {
    if (!procfs_is_path(path) || buffer == NULL || capacity == 0) return -14;
    size_t length = 0;
    process_t *process = process_current();
    if (path_eq(path, "/proc/uptime")) {
        uint64_t seconds = timer_ticks() / timer_frequency();
        return append_u64(buffer, capacity, &length, seconds) && append_text(buffer, capacity, &length, "\n") ? (ssize_t)length : -28;
    }
    if (path_eq(path, "/proc/meminfo")) {
        bool ok = append_text(buffer, capacity, &length, "MemTotal: ") && append_u64(buffer, capacity, &length, pmm_total_pages() * (NPK_PAGE_SIZE / 1024)) &&
                  append_text(buffer, capacity, &length, " kB\nMemFree: ") && append_u64(buffer, capacity, &length, pmm_free_pages() * (NPK_PAGE_SIZE / 1024)) &&
                  append_text(buffer, capacity, &length, " kB\n");
        return ok ? (ssize_t)length : -28;
    }
    if (path_eq(path, "/proc/mounts")) {
        return append_text(buffer, capacity, &length, "proc /proc proc ro 0 0\ninitramfs / cpio ro 0 0\n") ? (ssize_t)length : -28;
    }
    if (path_eq(path, "/proc/self/status")) {
        bool ok = append_text(buffer, capacity, &length, "Name:\tNPKernel\nState:\t") &&
                  append_text(buffer, capacity, &length, process && process->alive ? "R\nPid:\t" : "Z\nPid:\t") &&
                  append_u64(buffer, capacity, &length, process ? process->pid : 1) &&
                  append_text(buffer, capacity, &length, "\nThreads:\t1\n");
        return ok ? (ssize_t)length : -28;
    }
    if (path_eq(path, "/proc") || path_eq(path, "/proc/")) return append_text(buffer, capacity, &length, "self\nmeminfo\nuptime\nmounts\n") ? (ssize_t)length : -28;
    return -2;
}
