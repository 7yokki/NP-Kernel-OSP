typedef unsigned long size_t;
typedef long ssize_t;

static long syscall3(long number, long arg1, long arg2, long arg3) {
    long result;
    __asm__ volatile("syscall"
                     : "=a"(result)
                     : "a"(number), "D"(arg1), "S"(arg2), "d"(arg3)
                     : "rcx", "r11", "memory");
    return result;
}

__attribute__((noreturn)) static void syscall_exit(int status) {
    (void)syscall3(60, status, 0, 0);
    for (;;) __asm__ volatile("pause" ::: "memory");
}

__attribute__((noreturn, used)) void _start(void) {
    static const char message[] = "C_ELF_EXIT_TEST: write succeeded; exiting now\n";
    ssize_t written = (ssize_t)syscall3(1, 1, (long)message, (long)(sizeof(message) - 1));
    syscall_exit(written == (ssize_t)(sizeof(message) - 1) ? 0 : 1);
}
