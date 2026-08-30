#ifndef NPK_SYSCALL_H
#define NPK_SYSCALL_H

#include "types.h"

/* Linux x86_64 syscall calling convention: rax number; rdi,rsi,rdx,r10,r8,r9 args. */
typedef struct syscall_frame {
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t rip, cs, rflags, rsp, ss;
} syscall_frame_t;

enum {
    NPK_SYS_READ = 0,
    NPK_SYS_WRITE = 1,
    NPK_SYS_OPEN = 2,
    NPK_SYS_CLOSE = 3,
    NPK_SYS_FSYNC = 74,
    NPK_SYS_FDATASYNC = 75,
    NPK_SYS_STAT = 4,
    NPK_SYS_FSTAT = 5,
    NPK_SYS_LSEEK = 8,
    NPK_SYS_RT_SIGQUEUEINFO = 129,
    NPK_SYS_POLL = 7,
    NPK_SYS_MMAP = 9,
    NPK_SYS_MPROTECT = 10,
    NPK_SYS_MUNMAP = 11,
    NPK_SYS_READV = 19,
    NPK_SYS_WRITEV = 20,
    NPK_SYS_PIPE = 22,
    NPK_SYS_DUP = 32,
    NPK_SYS_DUP2 = 33,
    NPK_SYS_SCHED_YIELD = 24,
    NPK_SYS_BRK = 12,
    NPK_SYS_RT_SIGACTION = 13,
    NPK_SYS_RT_SIGPROCMASK = 14,
    NPK_SYS_RT_SIGRETURN = 15,
    NPK_SYS_IOCTL = 16,
    NPK_SYS_NANOSLEEP = 35,
    NPK_SYS_GETPID = 39,
    NPK_SYS_GETCWD = 79,
    NPK_SYS_READLINK = 89,
    NPK_SYS_FORK = 57,
    NPK_SYS_CLONE = 56,
    NPK_SYS_EXECVE = 59,
    NPK_SYS_EXIT = 60,
    NPK_SYS_WAIT4 = 61,
    NPK_SYS_WAITPID = 114,
    NPK_SYS_KILL = 62,
    NPK_SYS_TGKILL = 234,
    NPK_SYS_UNAME = 63,
    NPK_SYS_ARCH_PRCTL = 158,
    NPK_SYS_GETTID = 186,
    NPK_SYS_CLOCK_GETTIME = 228,
    NPK_SYS_EXIT_GROUP = 231,
    NPK_SYS_GETDENTS64 = 217,
    NPK_SYS_FCNTL = 72,
    NPK_SYS_FUTEX = 202,
    NPK_SYS_EPOLL_WAIT = 232,
    NPK_SYS_EPOLL_CTL = 233,
    NPK_SYS_PPOLL = 271,
    NPK_SYS_EPOLL_CREATE1 = 291,
    NPK_SYS_RT_TGSIGQUEUEINFO = 297,
    NPK_SYS_SET_TID_ADDRESS = 218,
    NPK_SYS_SYNC = 162,
    NPK_SYS_OPENAT = 257,
    /* NPKernel extension namespace; not Linux syscall numbers. */
    NPK_SYS_NPK_SHM_CREATE = 0x400,
    NPK_SYS_NPK_DISPLAY_INFO = 0x403,
    NPK_SYS_NPK_DISPLAY_CLAIM = 0x404,
    NPK_SYS_NPK_DISPLAY_MAP = 0x405,
    NPK_SYS_NPK_DISPLAY_RELEASE = 0x406,
    NPK_SYS_NPK_INPUT_READ = 0x407,
    NPK_SYS_NPK_FD_SEND = 0x401,
    NPK_SYS_NPK_FD_RECV = 0x402,
};

void syscall_init(void);
uint64_t syscall_dispatch(syscall_frame_t *frame);

#endif
