#ifndef NPK_PROCESS_H
#define NPK_PROCESS_H

#include "types.h"

#define NPK_PROCESS_PATH_MAX 256U

struct npk_vma;
struct syscall_frame;

#define NPK_MAX_FDS 32
#define NPK_KERNEL_STACK_SIZE 16384U
#define NPK_USER_STACK_TOP 0x00007ffffff00000ULL
#define NPK_USER_STACK_PAGES 32U
#define NPK_SIGNAL_MAX 64U
#define NPK_SIGRTMIN 32U
#define NPK_SIGRTMAX 64U
#define NPK_SIGNAL_QUEUE_MAX 32U
#define NPK_FD_TRANSFER_MAX 8U

typedef struct {
    uint64_t handler;
    uint64_t flags;
    uint64_t restorer;
    uint64_t mask[16];
} npk_sigaction_t;

typedef struct {
    uint64_t signal;
    int64_t code;
    int64_t sender_pid;
    uint32_t sender_uid;
    int64_t value;
} npk_signal_info_t;

typedef enum { THREAD_UNUSED, THREAD_READY, THREAD_RUNNING, THREAD_BLOCKED, THREAD_ZOMBIE } thread_state_t;

typedef struct {
    uint64_t rip, rsp, rflags;
    uint64_t rax, rbx, rcx, rdx, rsi, rdi, rbp;
    uint64_t r8, r9, r10, r11, r12, r13, r14, r15;
    uint64_t cr3;
} cpu_context_t;

typedef struct thread {
    uint64_t tid;
    thread_state_t state;
    uint8_t privilege_ring;
    cpu_context_t context;
    uint64_t kernel_stack;
    uint64_t kernel_stack_top;
    uint64_t user_stack_top;
    /* User-mode TLS base installed in IA32_FS_BASE on activation. */
    uint64_t fs_base;
    uint64_t gs_base;
    /* Points into this thread's kernel stack while it is preempted. The
     * layout is GPR pushes followed by the CPU IRET frame. */
    uint64_t *saved_interrupt_frame;
    uint8_t saved_frame_kind; /* 0: IRQ/IRET frame, 1: SYSCALL frame */
    /* Synchronous user-signal delivery state. The saved array uses the same
     * 15-GPR + 5-qword IRET layout as an interrupt wrapper frame. */
    uint8_t signal_inflight;
    uint64_t signal_frame_user;
    uint64_t signal_restorer;
    uint64_t signal_number;
    uint64_t signal_blocked;
    uint64_t signal_pending;
    uint64_t signal_saved_mask;
    npk_signal_info_t signal_queue[NPK_SIGNAL_QUEUE_MAX];
    uint8_t signal_queue_head;
    uint8_t signal_queue_tail;
    uint8_t signal_queue_count;
    uint64_t signal_saved_frame[20];
    /* Futex wait state is kept on the thread so a waiter slot may be
     * recycled immediately when another thread wakes or times it out. */
    uint8_t futex_waiting;
    int64_t futex_result;
    vaddr_t clear_child_tid;
    uint8_t clone_thread;
    uint8_t group_leader;
    uint8_t thread_exit_status;
    void (*entry)(void *argument);
    void *argument;
    struct process *owner;
    struct thread *next;
} thread_t;

typedef struct process {
    uint64_t pid;
    uint64_t parent_pid;
    uint32_t thread_count;
    uint8_t alive;
    uint8_t exit_status;
    uint8_t group_exiting;
    uint8_t waited;
    uint8_t wait_active;
    int64_t wait_target;
    vaddr_t wait_status_user;
    uint64_t wait_options;
    int64_t wait_result;
    uint8_t wait_woken;
    uint64_t address_space_root;
    uint64_t image_base;
    uint64_t image_end;
    uint64_t mmap_cursor;
    uint64_t brk_start;
    uint64_t brk_end;
    uint64_t user_stack_top;
    uint64_t user_stack_bottom;
    uint64_t user_stack_limit;
    char image_path[NPK_PROCESS_PATH_MAX];
    struct {
        int handle;
        uint32_t fd_flags;
    } incoming_fd_transfers[NPK_FD_TRANSFER_MAX];
    uint32_t incoming_fd_count;
    struct npk_vma *vmas;
    thread_t *main_thread;
    npk_sigaction_t signal_actions[NPK_SIGNAL_MAX + 1];
    int fds[NPK_MAX_FDS];
    uint32_t fd_flags[NPK_MAX_FDS];
} process_t;

void process_init(void);
process_t *process_create_user(paddr_t address_space_root, uint64_t entry, uint64_t stack_top);
process_t *process_exec_image(const void *image, size_t image_size);
int64_t process_exec_current(const void *image, size_t image_size,
                             const char *const *argv, size_t argc,
                             const char *const *envp, size_t envc,
                             struct syscall_frame *frame);
process_t *process_exec_image_with_args(const void *image, size_t image_size,
                                        const char *const *argv, size_t argc,
                                        const char *const *envp, size_t envc);
void process_activate(process_t *process);
void process_exit_current(uint8_t status);
int64_t process_wait4(int64_t target_pid, vaddr_t user_status, uint64_t options);
thread_t *scheduler_current(void);
process_t *process_current(void);
void process_set_image_path(process_t *process, const char *path);
int64_t process_send_fd(int64_t target_pid, int source_fd);
int process_receive_fd(process_t *process, int *handle, uint32_t *fd_flags);
void scheduler_tick(void);
/* Called from the PIT IRQ with the complete wrapper frame. It returns the
 * frame pointer that the assembly wrapper must restore before IRETQ. */
uint64_t *scheduler_preempt(uint64_t *interrupt_frame);
void scheduler_yield(void);
bool scheduler_reschedule_pending(void);
thread_t *scheduler_create_kernel_thread(void (*entry)(void *), void *argument);
void context_switch(cpu_context_t *old_context, const cpu_context_t *new_context);
void process_launch_user(process_t *process);
int64_t process_fork(const struct syscall_frame *parent_frame);
int64_t process_clone(uint64_t flags, vaddr_t child_stack, vaddr_t parent_tid,
                      vaddr_t tls, vaddr_t child_tid, const struct syscall_frame *parent_frame);
void process_exit_thread_current(uint8_t status);
int64_t process_set_tid_address(vaddr_t clear_child_tid);

/* Deliver a synchronous signal into the currently faulting user thread. The
 * frame is the complete wrapper frame: 15 saved GPRs followed by RIP, CS,
 * RFLAGS, RSP and SS. */
bool process_deliver_signal(uint64_t *frame, uint64_t signal);
bool process_deliver_pending_signal(uint64_t *frame);
bool process_deliver_pending_signal_syscall(struct syscall_frame *frame);
uint64_t process_sigreturn(struct syscall_frame *frame);
int64_t process_send_signal(int64_t pid, uint64_t signal);
int64_t process_send_thread_signal(int64_t tgid, int64_t tid, uint64_t signal);
int64_t process_send_signal_info(int64_t pid, uint64_t signal, const npk_signal_info_t *info);
int64_t process_send_thread_signal_info(int64_t tgid, int64_t tid, uint64_t signal,
                                        const npk_signal_info_t *info);
uint64_t process_get_signal_mask(void);
void process_set_signal_mask(uint64_t mask);
__attribute__((noreturn)) void process_terminate_by_signal(uint64_t signal);

/* Basic Linux futex ABI used by static thread runtimes. The wait path
 * validates the user word, compares it before blocking, and wakes only
 * waiters in the same address space. */
int64_t process_futex_wait(vaddr_t user_address, uint32_t expected, vaddr_t user_timeout);
int64_t process_futex_wake(vaddr_t user_address, uint32_t count);

#endif
