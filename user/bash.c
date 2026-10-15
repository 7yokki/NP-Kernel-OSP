#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/utsname.h>
#include <unistd.h>

#define SHELL_LINE_MAX 512
#define SHELL_ARG_MAX 32
#define IO_BUFFER_SIZE 512
#define CURSOR_BLINK_MS 1000
#define BASH_JOB_MAX 16
#define BASH_WAIT_NOHANG 1

static npk_termios_t saved_terminal;
static int terminal_raw;
static int running_in_child;
static pid_t background_jobs[BASH_JOB_MAX];

static void terminal_restore(void) {
    if (!terminal_raw) return;
    (void)ioctl(0, TCSETS, &saved_terminal);
    terminal_raw = 0;
}

static void terminal_enable_raw(void) {
    if (ioctl(0, TCGETS, &saved_terminal) != 0) return;
    npk_termios_t raw = saved_terminal;
    raw.c_lflag &= ~(TTY_ICANON | TTY_ECHO);
    if (ioctl(0, TCSETS, &raw) == 0) terminal_raw = 1;
}

static void erase_prompt_cursor(void) { fputs("\b \b", stdout); }

static int read_command_line(char *line, size_t capacity) {
    size_t length = 0;
    unsigned char utf8_pending[4];
    size_t utf8_length = 0;
    size_t utf8_expected = 0;
    int cursor_visible = 1;
    if (capacity == 0) return -1;
    line[0] = '\0';
    putchar('_');
    for (;;) {
        struct pollfd input = { .fd = 0, .events = POLLIN, .revents = 0 };
        int ready = poll(&input, 1, CURSOR_BLINK_MS);
        if (ready < 0) continue;
        if (ready == 0) {
            if (cursor_visible) erase_prompt_cursor();
            else putchar('_');
            cursor_visible = !cursor_visible;
            continue;
        }
        unsigned char c = 0;
        ssize_t count = read(0, &c, 1);
        if (count < 0) continue;
        if (count == 0) return length ? (line[length] = '\0', 1) : 0;
        if (cursor_visible) erase_prompt_cursor();
        cursor_visible = 0;
        if (c == '\r' || c == '\n') {
            putchar('\n');
            line[length] = '\0';
            return 1;
        }
        if (c == '\b' || c == 0x7f) {
            if (length != 0) {
                unsigned char removed = (unsigned char)line[--length];
                if ((removed & 0xc0U) == 0x80U) {
                    while (length != 0 && ((unsigned char)line[length - 1] & 0xc0U) == 0x80U) --length;
                    if (length != 0) --length; /* leading byte */
                }
                line[length] = '\0';
                utf8_length = utf8_expected = 0;
                fputs("\b \b", stdout);
            }
        } else if (c == 4 && length == 0) {
            putchar('\n');
            return 0;
        } else if (c >= 0x20 && c != 0x7f && length + 1 < capacity) {
            line[length++] = (char)c;
            line[length] = '\0';
            if (utf8_expected == 0) {
                if ((c & 0xe0U) == 0xc0U) utf8_expected = 2;
                else if ((c & 0xf0U) == 0xe0U) utf8_expected = 3;
                else if ((c & 0xf8U) == 0xf0U) utf8_expected = 4;
                else putchar(c);
                if (utf8_expected) {
                    utf8_pending[0] = c;
                    utf8_length = 1;
                }
            } else if ((c & 0xc0U) == 0x80U && utf8_length < sizeof(utf8_pending)) {
                utf8_pending[utf8_length++] = c;
                if (utf8_length == utf8_expected) {
                    (void)write(1, utf8_pending, utf8_length);
                    utf8_length = utf8_expected = 0;
                }
            } else {
                utf8_length = utf8_expected = 0;
                putchar(c);
            }
        }
        putchar('_');
        cursor_visible = 1;
    }
}

static int is_space(char c) { return c == ' ' || c == '\t' || c == '\r' || c == '\n'; }
static int contains_slash(const char *text) { return strchr(text, '/') != NULL; }
static int ends_with_elf(const char *text) {
    size_t n = strlen(text);
    return n >= 4 && strcmp(text + n - 4, ".elf") == 0;
}

/* A bounded tokenizer with single/double quotes and backslash escaping. */
static int parse_line(char *line, char **argv, int capacity) {
    char *read = line;
    char *write = line;
    int argc = 0;
    while (*read) {
        while (is_space(*read)) ++read;
        if (!*read || *read == '#') break;
        if (argc + 1 >= capacity) return -1;
        argv[argc++] = write;
        char quote = 0;
        while (*read) {
            char c = *read++;
            if (quote) {
                if (c == quote) { quote = 0; continue; }
                if (c == '\\' && quote == '"' && *read) c = *read++;
                *write++ = c;
                continue;
            }
            if (c == '\'' || c == '"') { quote = c; continue; }
            if (c == '\\' && *read) { *write++ = *read++; continue; }
            if (is_space(c)) break;
            *write++ = c;
        }
        if (quote) return -1;
        *write++ = '\0';
    }
    argv[argc] = NULL;
    return argc;
}

static int copy_file_to_stdout(const char *path) {
    int fd = open(path, O_RDONLY);
    if (fd < 0) {
        fprintf(stderr, "cat: %s: open failed (errno=%d)\n", path, errno);
        return 1;
    }
    char buffer[IO_BUFFER_SIZE];
    int result = 0;
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0) { fprintf(stderr, "cat: %s: read failed\n", path); result = 1; break; }
        if (count == 0) break;
        ssize_t written = write(1, buffer, (size_t)count);
        if (written != count) { fprintf(stderr, "cat: %s: write failed\n", path); result = 1; break; }
    }
    close(fd);
    return result;
}

static int run_ls(int argc, char **argv) {
    int show_all = 0;
    const char *path = ".";
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "-a") == 0) show_all = 1;
        else if (argv[i][0] == '-' && argv[i][1] != '\0') {
            fprintf(stderr, "ls: unsupported option: %s\n", argv[i]);
            return 2;
        } else path = argv[i];
    }
    DIR *directory = opendir(path);
    if (!directory) { fprintf(stderr, "ls: %s: open failed (errno=%d)\n", path, errno); return 1; }
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!show_all && entry->d_name[0] == '.') continue;
        printf("%c %s\n", entry->d_type == DT_DIR ? 'd' : '-', entry->d_name);
    }
    closedir(directory);
    return 0;
}

static int write_all(int fd, const void *buffer, size_t count) {
    const unsigned char *bytes = (const unsigned char *)buffer;
    size_t written = 0;
    while (written < count) {
        ssize_t result = write(fd, bytes + written, count - written);
        if (result <= 0) return -1;
        written += (size_t)result;
    }
    return 0;
}

static int run_head(int argc, char **argv) {
    long lines = 10;
    int argument = 1;
    if (argument < argc && strcmp(argv[argument], "-n") == 0) {
        if (argument + 2 >= argc) { puts("head: -n requires a count and path"); return 2; }
        lines = strtol(argv[argument + 1], NULL, 10);
        if (lines < 0) { puts("head: line count must be non-negative"); return 2; }
        argument += 2;
    }
    if (argument + 1 != argc) { puts("head: expected one file path"); return 2; }
    int fd = open(argv[argument], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "head: %s: open failed (errno=%d)\n", argv[argument], errno); return 1; }
    char buffer[IO_BUFFER_SIZE];
    int result = 0;
    while (lines > 0) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0) { result = 1; break; }
        if (count == 0) break;
        for (ssize_t i = 0; i < count; ++i) {
            if (write_all(1, &buffer[i], 1) != 0) { result = 1; lines = 0; break; }
            if (buffer[i] == '\n' && --lines == 0) break;
        }
    }
    if (close(fd) != 0) result = 1;
    if (result) fprintf(stderr, "head: %s: read/write failed\n", argv[argument]);
    return result;
}

static int run_wc(int argc, char **argv) {
    if (argc != 2) { puts("wc: expected one file path"); return 2; }
    int fd = open(argv[1], O_RDONLY);
    if (fd < 0) { fprintf(stderr, "wc: %s: open failed (errno=%d)\n", argv[1], errno); return 1; }
    unsigned long lines = 0, words = 0, bytes = 0;
    int in_word = 0;
    char buffer[IO_BUFFER_SIZE];
    int result = 0;
    for (;;) {
        ssize_t count = read(fd, buffer, sizeof(buffer));
        if (count < 0) { result = 1; break; }
        if (count == 0) break;
        bytes += (unsigned long)count;
        for (ssize_t i = 0; i < count; ++i) {
            if (buffer[i] == '\n') ++lines;
            if (is_space(buffer[i])) in_word = 0;
            else if (!in_word) { ++words; in_word = 1; }
        }
    }
    if (close(fd) != 0) result = 1;
    if (result) { fprintf(stderr, "wc: %s: read failed\n", argv[1]); return result; }
    printf("%lu %lu %lu %s\n", lines, words, bytes, argv[1]);
    return 0;
}

static int run_stat(int argc, char **argv) {
    if (argc != 2) { puts("stat: expected one path"); return 2; }
    struct stat metadata;
    if (stat(argv[1], &metadata) != 0) {
        fprintf(stderr, "stat: %s: metadata unavailable (errno=%d)\n", argv[1], errno);
        return 1;
    }
    printf("%s: %s, %lu bytes, mode 0x%x\n", argv[1],
           S_ISDIR(metadata.st_mode) ? "directory" : "file",
           (unsigned long)metadata.st_size, (unsigned)metadata.st_mode);
    return 0;
}

static int run_touch(int argc, char **argv) {
    if (argc < 2) { puts("touch: provide one or more writable file paths (for example /persist/name)"); return 2; }
    int result = 0;
    for (int i = 1; i < argc; ++i) {
        int fd = open(argv[i], O_WRONLY | O_CREAT, 0644);
        if (fd < 0) { fprintf(stderr, "touch: %s: create failed (errno=%d)\n", argv[i], errno); result = 1; }
        else if (close(fd) != 0) { fprintf(stderr, "touch: %s: close failed\n", argv[i]); result = 1; }
    }
    return result;
}

static int run_cp(int argc, char **argv) {
    if (argc != 3) { puts("cp: expected source and destination paths"); return 2; }
    int source = open(argv[1], O_RDONLY);
    if (source < 0) { fprintf(stderr, "cp: %s: open failed (errno=%d)\n", argv[1], errno); return 1; }
    int destination = open(argv[2], O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (destination < 0) {
        fprintf(stderr, "cp: %s: create failed (errno=%d)\n", argv[2], errno);
        close(source);
        return 1;
    }
    char buffer[IO_BUFFER_SIZE];
    int result = 0;
    for (;;) {
        ssize_t count = read(source, buffer, sizeof(buffer));
        if (count < 0) { result = 1; break; }
        if (count == 0) break;
        if (write_all(destination, buffer, (size_t)count) != 0) { result = 1; break; }
    }
    if (close(source) != 0) result = 1;
    if (close(destination) != 0) result = 1;
    if (result) fprintf(stderr, "cp: copy failed: %s -> %s\n", argv[1], argv[2]);
    return result;
}

static int run_uname(int argc, char **argv) {
    struct utsname name;
    if (uname(&name) != 0) { fprintf(stderr, "uname: syscall failed\n"); return 1; }
    int all = argc == 1;
    for (int i = 1; i < argc; ++i) if (strcmp(argv[i], "-a") == 0) all = 1;
    if (all) {
        printf("%s %s %s %s %s\n", name.sysname, name.nodename, name.release, name.version, name.machine);
        return 0;
    }
    int printed = 0;
    for (int i = 1; i < argc; ++i) {
        const char *field = NULL;
        if (strcmp(argv[i], "-s") == 0) field = name.sysname;
        else if (strcmp(argv[i], "-n") == 0) field = name.nodename;
        else if (strcmp(argv[i], "-r") == 0) field = name.release;
        else if (strcmp(argv[i], "-v") == 0) field = name.version;
        else if (strcmp(argv[i], "-m") == 0) field = name.machine;
        else if (strcmp(argv[i], "-o") == 0) field = name.sysname;
        else { fprintf(stderr, "uname: unsupported option: %s\n", argv[i]); return 2; }
        if (printed++) putchar(' ');
        fputs(field, stdout);
    }
    if (printed) putchar('\n');
    return 0;
}

static void reap_background_jobs(void);
static int run_builtin_command(int argc, char **argv);

static int resolve_program_path(const char *program, char *path, size_t capacity) {
    int length;
    if (contains_slash(program)) length = snprintf(path, capacity, "%s", program);
    else if (ends_with_elf(program)) length = snprintf(path, capacity, "/bin/%s", program);
    else length = snprintf(path, capacity, "/bin/%s.elf", program);
    return length < 0 || (size_t)length >= capacity ? -1 : 0;
}

static int run_exec(int argc, char **argv) {
    if (argc < 2) { puts("exec: provide a program"); return 2; }
    char path[256];
    const char *program = argv[1];
    if (resolve_program_path(program, path, sizeof(path)) != 0) {
        fprintf(stderr, "exec: %s: path too long\n", program); return 126;
    }
    char *environment[] = { NULL };
    if (!running_in_child) terminal_restore();
    execve(path, &argv[1], environment);
    if (!running_in_child) terminal_enable_raw();
    fprintf(stderr, "exec: %s: failed (errno=%d)\n", program, errno);
    return errno == ENOENT ? 127 : 126;
}

static int is_builtin_name(const char *name) {
    static const char *const names[] = {
        "help", "echo", "clear", "pwd", "cd", "ls", "cat", "head", "wc", "stat",
        "touch", "cp", "uname", "free", "ps", "whoami", "true", "false", "exit",
        "exec", "jobs", "wait", NULL
    };
    for (size_t i = 0; names[i]; ++i) if (strcmp(name, names[i]) == 0) return 1;
    return 0;
}

static int remember_background_job(pid_t pid) {
    for (size_t i = 0; i < BASH_JOB_MAX; ++i) {
        if (background_jobs[i] == 0) { background_jobs[i] = pid; return 1; }
    }
    return 0;
}

static void reap_background_jobs(void) {
    for (size_t i = 0; i < BASH_JOB_MAX; ++i) {
        pid_t pid = background_jobs[i];
        if (pid <= 0) continue;
        int status = 0;
        pid_t result = waitpid(pid, &status, BASH_WAIT_NOHANG);
        if (result == pid) {
            printf("[%d] done (%d)\n", (int)pid, (status >> 8) & 0xff);
            background_jobs[i] = 0;
        } else if (result < 0 && errno == ECHILD) {
            background_jobs[i] = 0;
        }
    }
}

static int wait_for_child(pid_t pid) {
    int status = 0;
    pid_t result;
    do { result = waitpid(pid, &status, 0); } while (result < 0 && errno == EINTR);
    if (result != pid) {
        fprintf(stderr, "bash: waitpid(%d) failed (errno=%d)\n", (int)pid, errno);
        return 127;
    }
    return (status >> 8) & 0xff;
}

static int run_jobs(void) {
    reap_background_jobs();
    int count = 0;
    for (size_t i = 0; i < BASH_JOB_MAX; ++i) {
        if (background_jobs[i] > 0) {
            printf("[%d] running\n", (int)background_jobs[i]);
            ++count;
        }
    }
    if (!count) puts("no background jobs");
    return 0;
}

static int run_wait_jobs(void) {
    int status = 0;
    for (size_t i = 0; i < BASH_JOB_MAX; ++i) {
        pid_t pid = background_jobs[i];
        if (pid <= 0) continue;
        status = wait_for_child(pid);
        background_jobs[i] = 0;
    }
    return status;
}

static int run_external(char **argv, int background) {
    char path[256];
    if (resolve_program_path(argv[0], path, sizeof(path)) != 0) {
        fprintf(stderr, "%s: program path too long\n", argv[0]);
        return 126;
    }
    int restore_raw = terminal_raw && !background;
    if (restore_raw) terminal_restore();
    pid_t pid = fork();
    if (pid < 0) {
        fprintf(stderr, "%s: fork failed (errno=%d)\n", argv[0], errno);
        if (restore_raw) terminal_enable_raw();
        return 126;
    }
    if (pid == 0) {
        running_in_child = 1;
        char *environment[] = { NULL };
        execve(path, argv, environment);
        fprintf(stderr, "%s: exec failed (errno=%d)\n", argv[0], errno);
        _exit(errno == ENOENT ? 127 : 126);
    }
    if (background) {
        if (!remember_background_job(pid)) {
            puts("bash: job table full; waiting for the new process");
            return wait_for_child(pid);
        }
        printf("[%d] started %s\n", (int)pid, argv[0]);
        return 0;
    }
    int status = wait_for_child(pid);
    if (restore_raw) terminal_enable_raw();
    return status;
}

static int run_builtin_command(int argc, char **argv) {
    if (argc == 0) return 0;
    if (strcmp(argv[0], "help") == 0) {
        puts("NPKernel bash-like shell (not GNU Bash)");
        puts("Builtins: help echo clear pwd cd ls cat head wc stat touch cp uname free ps whoami true false exit exec jobs wait");
        puts("Startup: bash -c COMMAND or bash SCRIPT; script commands run one per line.");
        puts("External ELF commands run as child processes; append & to run one in the background.");
        puts("Writable files are limited to /persist; mkdir/rm/mv and shell redirection are not supported.");
        puts("Use exec /bin/<program>.elf to replace the shell; the prompt has a blinking underscore cursor.");
        return 0;
    }
    if (strcmp(argv[0], "echo") == 0) {
        int start = argc > 1 && strcmp(argv[1], "-n") == 0 ? 2 : 1;
        for (int i = start; i < argc; ++i) { if (i != start) putchar(' '); fputs(argv[i], stdout); }
        if (start == 1 || argc > 1) {
            if (!(argc > 1 && strcmp(argv[1], "-n") == 0)) putchar('\n');
        }
        return 0;
    }
    if (strcmp(argv[0], "clear") == 0) { printf("\033[2J\033[H"); return 0; }
    if (strcmp(argv[0], "pwd") == 0) {
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd))) { fprintf(stderr, "pwd: getcwd failed\n"); return 1; }
        puts(cwd); return 0;
    }
    if (strcmp(argv[0], "cd") == 0) {
        const char *path = argc > 1 ? argv[1] : "/";
        if (argc > 2) { puts("cd: expected at most one path"); return 2; }
        if (chdir(path) != 0) { fprintf(stderr, "cd: %s: chdir failed (errno=%d)\n", path, errno); return 1; }
        return 0;
    }
    if (strcmp(argv[0], "ls") == 0) return run_ls(argc, argv);
    if (strcmp(argv[0], "head") == 0) return run_head(argc, argv);
    if (strcmp(argv[0], "wc") == 0) return run_wc(argc, argv);
    if (strcmp(argv[0], "stat") == 0) return run_stat(argc, argv);
    if (strcmp(argv[0], "touch") == 0) return run_touch(argc, argv);
    if (strcmp(argv[0], "cp") == 0) return run_cp(argc, argv);
    if (strcmp(argv[0], "cat") == 0) {
        if (argc < 2) { puts("cat: provide one or more file paths"); return 2; }
        int result = 0;
        for (int i = 1; i < argc; ++i) if (copy_file_to_stdout(argv[i]) != 0) result = 1;
        return result;
    }
    if (strcmp(argv[0], "uname") == 0) return run_uname(argc, argv);
    if (strcmp(argv[0], "free") == 0) return copy_file_to_stdout("/proc/meminfo");
    if (strcmp(argv[0], "ps") == 0) return copy_file_to_stdout("/proc/self/status");
    if (strcmp(argv[0], "whoami") == 0) { puts("root"); return 0; }
    if (strcmp(argv[0], "true") == 0) return 0;
    if (strcmp(argv[0], "false") == 0) return 1;
    if (strcmp(argv[0], "exit") == 0) { terminal_restore(); exit(argc > 1 ? atoi(argv[1]) : 0); }
    if (strcmp(argv[0], "exec") == 0) return run_exec(argc, argv);
    if (strcmp(argv[0], "jobs") == 0) return run_jobs();
    if (strcmp(argv[0], "wait") == 0) {
        if (argc != 1) { puts("wait: this shell waits for all tracked background jobs"); return 2; }
        return run_wait_jobs();
    }
    return 127;
}

static int run_command(int argc, char **argv, int background) {
    if (argc == 0) return 0;
    if (is_builtin_name(argv[0])) {
        if (!background) return run_builtin_command(argc, argv);
        pid_t pid = fork();
        if (pid < 0) { fprintf(stderr, "%s: fork failed (errno=%d)\n", argv[0], errno); return 126; }
        if (pid == 0) {
            running_in_child = 1;
            _exit(run_builtin_command(argc, argv));
        }
        if (!remember_background_job(pid)) {
            puts("bash: job table full; waiting for the new process");
            return wait_for_child(pid);
        }
        printf("[%d] started %s\n", (int)pid, argv[0]);
        return 0;
    }
    return run_external(argv, background);
}

static int execute_script_segment(char *line, char **argv, int previous_status, int background) {
    int argc = parse_line(line, argv, SHELL_ARG_MAX);
    if (argc < 0) {
        puts("bash: invalid quoting or too many arguments");
        return 2;
    }
    if (argc == 0) return previous_status;
    return run_command(argc, argv, background);
}

/* Interpret bounded command segments separated by semicolons or line breaks.
 * Quotes and backslash escapes keep separators inside an argument. */
static int run_command_text(const char *text, int status) {
    char line[SHELL_LINE_MAX];
    char *argv[SHELL_ARG_MAX];
    size_t length = 0;
    int overflow = 0;
    int escaped = 0;
    int comment = 0;
    int command_started = 0;
    char quote = 0;

    for (size_t i = 0;; ++i) {
        char c = text[i];
        int at_end = c == '\0';
        if (!at_end && !comment && !quote && !escaped && !command_started && c == '#')
            comment = 1;
        if (comment && !at_end && c != '\n' && c != '\r') {
            if (length + 1 < sizeof(line)) line[length++] = c;
            else overflow = 1;
            continue;
        }
        int separator = at_end || (quote == 0 && !escaped &&
                                   (c == ';' || c == '\n' || c == '\r' || c == '&'));
        if (separator) {
            line[length] = '\0';
            if (overflow) {
                puts("bash: command exceeds line limit");
                status = 2;
            } else {
                status = execute_script_segment(line, argv, status, c == '&');
            }
            length = 0;
            overflow = 0;
            escaped = 0;
            comment = 0;
            command_started = 0;
            quote = 0;
            if (at_end) return status;
            continue;
        }

        if (length + 1 < sizeof(line)) line[length++] = c;
        else overflow = 1;
        if (escaped) {
            escaped = 0;
        } else if (quote == '"') {
            if (c == '"') quote = 0;
            else if (c == '\\') escaped = 1;
        } else if (quote == '\'') {
            if (c == '\'') quote = 0;
        } else {
            if (c == '\\') escaped = 1;
            else if (c == '\'' || c == '"') quote = c;
        }
        if (!is_space(c)) command_started = 1;
    }
}

static int run_script_file(const char *path) {
    FILE *script = fopen(path, "r");
    if (!script) {
        fprintf(stderr, "bash: %s: cannot open script (errno=%d)\n", path, errno);
        return 127;
    }
    char line[SHELL_LINE_MAX];
    size_t length = 0;
    int overflow = 0;
    int status = 0;
    for (;;) {
        int character = fgetc(script);
        if (character == EOF || character == '\n' || character == '\r') {
            if (length != 0 || overflow) {
                line[length] = '\0';
                if (overflow) {
                    puts("bash: script line exceeds line limit");
                    status = 2;
                } else {
                    status = run_command_text(line, status);
                }
                length = 0;
                overflow = 0;
            }
            if (character == EOF) {
                if (ferror(script)) {
                    fprintf(stderr, "bash: %s: script read failed\n", path);
                    status = 1;
                }
                break;
            }
            continue;
        }
        if (length + 1 < sizeof(line)) line[length++] = (char)character;
        else overflow = 1;
    }
    if (fclose(script) != 0 && status == 0) {
        fprintf(stderr, "bash: %s: script close failed\n", path);
        status = 1;
    }
    return status;
}

int bash_main(int argc, char **argv) {
    if (argc > 1) {
        if (strcmp(argv[1], "-c") == 0) {
            if (argc < 3) {
                puts("bash: -c requires a command string");
                return 2;
            }
            return run_command_text(argv[2], 0);
        }
        int script_index = 1;
        if (strcmp(argv[script_index], "--") == 0) ++script_index;
        else if (argv[script_index][0] == '-') {
            fprintf(stderr, "bash: unsupported option: %s\n", argv[script_index]);
            return 2;
        }
        if (script_index >= argc) {
            puts("bash: provide a script path");
            return 2;
        }
        return run_script_file(argv[script_index]);
    }

    char line[SHELL_LINE_MAX];
    int last_status = 0;
    terminal_enable_raw();
    for (;;) {
        reap_background_jobs();
        char cwd[256];
        if (!getcwd(cwd, sizeof(cwd))) strcpy(cwd, "?");
        printf("npk:%s# ", cwd);
        if (terminal_raw) {
            if (!read_command_line(line, sizeof(line))) { terminal_restore(); exit(0); }
        } else if (!fgets(line, sizeof(line), stdin)) {
            exit(0);
        }
        last_status = run_command_text(line, last_status);
    }
}
