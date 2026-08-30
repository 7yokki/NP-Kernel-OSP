#include <npk/arch.h>
#include <npk/elf.h>
#include <npk/gop.h>
#include <npk/heap.h>
#include <npk/log.h>
#include <npk/memory.h>
#include <npk/panic.h>
#include <npk/process.h>
#include <npk/string.h>
#include <npk/syscall.h>
#include <npk/timer.h>
#include <npk/vm.h>
#include <npk/vfs.h>

extern void syscall_set_current_process(process_t *process);
extern void restore_interrupt_frame(uint64_t *frame);
extern void restore_user_frame_from_kernel(uint64_t *frame);
extern void enter_user_mode(uint64_t entry, uint64_t stack_top);
extern void arch_halt(void);

static process_t processes[NPK_MAX_PROCESSES];
static thread_t threads[NPK_MAX_THREADS];
static uint64_t next_pid = 1;
static uint64_t next_tid = 1;
static thread_t *current;
static process_t *kernel_process;
static uint64_t scheduler_ticks;
static bool reschedule_pending;
static paddr_t kernel_address_space_root;

#define INTERRUPT_GPR_QWORDS 15U
#define INTERRUPT_KERNEL_QWORDS 18U
#define INTERRUPT_USER_QWORDS 20U
#define KERNEL_CS 0x08ULL
#define USER_CS 0x23ULL
#define USER_SS 0x1bULL
#define PROCESS_EFAULT 14
#define PROCESS_EBADF 9
#define PROCESS_ECHILD 10
#define PROCESS_EAGAIN 11
#define PROCESS_EINVAL 22
#define PROCESS_ESRCH 3
#define PROCESS_EPERM 1
#define PROCESS_ENOSYS 38
#define PROCESS_EINTR 4
#define PROCESS_ENOMEM 12
#define PROCESS_ETIMEDOUT 110
#define PROCESS_WNOHANG 1ULL
#define NPK_FUTEX_MAX_WAITERS NPK_MAX_THREADS
#define NPK_FUTEX_PRIVATE_FLAG 128U
#define NPK_FUTEX_WAIT 0U
#define NPK_FUTEX_WAKE 1U
#define NPK_CLONE_VM 0x0000000000000100ULL
#define NPK_CLONE_FS 0x0000000000000200ULL
#define NPK_CLONE_FILES 0x0000000000000400ULL
#define NPK_CLONE_SIGHAND 0x0000000000000800ULL
#define NPK_CLONE_THREAD 0x0000000000010000ULL
#define NPK_CLONE_SETTLS 0x00000000000080000ULL
#define NPK_CLONE_PARENT_SETTID 0x0000000010000000ULL
#define NPK_CLONE_CHILD_CLEARTID 0x0000000020000000ULL
#define NPK_CLONE_CHILD_SETTID 0x0000000100000000ULL
#define NPK_CLONE_SYSVSEM 0x0000000000040000ULL
#define NPK_CLONE_THREAD_FLAGS (NPK_CLONE_VM | NPK_CLONE_FS | NPK_CLONE_FILES | NPK_CLONE_SIGHAND | NPK_CLONE_THREAD)
#define NPK_CLONE_OPTIONAL_FLAGS (NPK_CLONE_SETTLS | NPK_CLONE_PARENT_SETTID | NPK_CLONE_CHILD_CLEARTID | NPK_CLONE_CHILD_SETTID | NPK_CLONE_SYSVSEM)

typedef struct {
    int64_t seconds;
    int64_t nanoseconds;
} futex_timespec_t;

typedef struct {
    bool active;
    thread_t *thread;
    paddr_t address_space_root;
    vaddr_t user_address;
    uint64_t deadline;
    bool timed;
} futex_waiter_t;

static futex_waiter_t futex_waiters[NPK_FUTEX_MAX_WAITERS];

static uint64_t *prepare_user_entry_frame(thread_t *thread, uint64_t entry, uint64_t user_rsp) {
    if (!thread || !thread->kernel_stack_top) return NULL;
    uint64_t *frame = (uint64_t *)(thread->kernel_stack_top - INTERRUPT_USER_QWORDS * sizeof(uint64_t));
    memset(frame, 0, INTERRUPT_USER_QWORDS * sizeof(uint64_t));
    frame[INTERRUPT_GPR_QWORDS + 0] = entry;
    frame[INTERRUPT_GPR_QWORDS + 1] = USER_CS;
    frame[INTERRUPT_GPR_QWORDS + 2] = 0x202; /* IF=1, IOPL=0: normal user execution. */
    frame[INTERRUPT_GPR_QWORDS + 3] = user_rsp;
    frame[INTERRUPT_GPR_QWORDS + 4] = USER_SS;
    thread->saved_interrupt_frame = frame;
    thread->saved_frame_kind = 0;
    return frame;
}

static uint64_t *prepare_fork_frame_with_stack(thread_t *thread, const syscall_frame_t *parent,
                                                   vaddr_t child_stack) {
    if (!thread || !parent || !thread->kernel_stack_top) return NULL;
    uint64_t *frame = (uint64_t *)(thread->kernel_stack_top - INTERRUPT_USER_QWORDS * sizeof(uint64_t));
    memset(frame, 0, INTERRUPT_USER_QWORDS * sizeof(uint64_t));
    frame[0] = parent->r15;
    frame[1] = parent->r14;
    frame[2] = parent->r13;
    frame[3] = parent->r12;
    frame[4] = parent->r11;
    frame[5] = parent->r10;
    frame[6] = parent->r9;
    frame[7] = parent->r8;
    frame[8] = parent->rbp;
    frame[9] = parent->rdi;
    frame[10] = parent->rsi;
    frame[11] = parent->rdx;
    frame[12] = parent->rcx;
    frame[13] = parent->rbx;
    frame[14] = 0; /* fork/clone returns zero in the child */
    frame[INTERRUPT_GPR_QWORDS + 0] = parent->rip;
    frame[INTERRUPT_GPR_QWORDS + 1] = parent->cs;
    frame[INTERRUPT_GPR_QWORDS + 2] = parent->rflags;
    frame[INTERRUPT_GPR_QWORDS + 3] = child_stack ? child_stack : parent->rsp;
    frame[INTERRUPT_GPR_QWORDS + 4] = parent->ss;
    thread->saved_interrupt_frame = frame;
    thread->saved_frame_kind = 0;
    return frame;
}

static uint64_t *prepare_fork_frame(thread_t *thread, const syscall_frame_t *parent) {
    return prepare_fork_frame_with_stack(thread, parent, 0);
}

static uint64_t *materialize_context_frame(thread_t *thread) {
    if (!thread || !thread->context.rip || !thread->context.rsp) return NULL;
    /* context_switch resumes with RET. Place an IRET frame immediately below
     * the saved return address and make the post-IRET RSP skip that address. */
    uint64_t frame_address = thread->context.rsp + sizeof(uint64_t) - INTERRUPT_KERNEL_QWORDS * sizeof(uint64_t);
    uint64_t *frame = (uint64_t *)frame_address;
    memset(frame, 0, INTERRUPT_KERNEL_QWORDS * sizeof(uint64_t));
    frame[0] = thread->context.r15;
    frame[1] = thread->context.r14;
    frame[2] = thread->context.r13;
    frame[3] = thread->context.r12;
    frame[4] = thread->context.r11;
    frame[5] = thread->context.r10;
    frame[6] = thread->context.r9;
    frame[7] = thread->context.r8;
    frame[8] = thread->context.rbp;
    frame[9] = thread->context.rdi;
    frame[10] = thread->context.rsi;
    frame[11] = thread->context.rdx;
    frame[12] = thread->context.rcx;
    frame[13] = thread->context.rbx;
    frame[14] = thread->context.rax;
    frame[15] = thread->context.rip;
    frame[16] = KERNEL_CS;
    frame[17] = thread->context.rflags ? thread->context.rflags : 0x202;
    thread->saved_interrupt_frame = frame;
    thread->saved_frame_kind = 0;
    return frame;
}

static bool wait_target_matches(int64_t target_pid, const process_t *child) {
    if (!child) return false;
    if (target_pid == -1) return true;
    return target_pid > 0 && (uint64_t)target_pid == child->pid;
}

static process_t *find_process_by_pid(uint64_t pid) {
    for (unsigned i = 1; i < NPK_MAX_PROCESSES; ++i)
        if (processes[i].pid == pid) return &processes[i];
    return NULL;
}

static bool copyout_process(process_t *process, vaddr_t destination,
                            const void *source, size_t length) {
    if (!process || !source || !vmm_is_user_range(destination, length)) return false;
    const uint8_t *bytes = (const uint8_t *)source;
    size_t copied = 0;
    while (copied < length) {
        vaddr_t address = destination + copied;
        paddr_t physical = vmm_lookup_root(process->address_space_root, address, VM_USER | VM_WRITE);
        if (!physical) {
            if (!vm_ensure_user_page(process, address, true)) return false;
            physical = vmm_lookup_root(process->address_space_root, address, VM_USER | VM_WRITE);
        }
        if (!physical) return false;
        size_t page_left = NPK_PAGE_SIZE - (size_t)(address & (NPK_PAGE_SIZE - 1));
        size_t amount = length - copied < page_left ? length - copied : page_left;
        memcpy((uint8_t *)phys_to_virt(physical), bytes + copied, amount);
        copied += amount;
    }
    return true;
}

#define NPK_SA_RESTORER 0x04000000ULL
#define NPK_SA_SIGINFO 0x00000004ULL
#define NPK_SIGNAL_FRAME_MAGIC 0x4e504b5349474652ULL

static uint64_t signal_bit(uint64_t signal) {
    return signal >= 1 && signal <= 64 ? (1ULL << (signal - 1)) : 0;
}

static bool signal_uncatchable(uint64_t signal) {
    return signal == 9 || signal == 19; /* SIGKILL and SIGSTOP. */
}

static bool signal_action_handler_valid(const npk_sigaction_t *action) {
    if (!action || action->handler == 0 || action->handler == 1) return false;
    if (!vmm_is_user_range(action->handler, 1)) return false;
    if (action->restorer == 0 || !vmm_is_user_range(action->restorer, 1)) return false;
    return true;
}

typedef struct {
    uint64_t magic;
    uint64_t signal;
    uint64_t siginfo;
    uint64_t ucontext;
} npk_user_signal_header_t;

#define NPK_USER_SIGINFO_SIZE 128U
#define NPK_USER_UCONTEXT_SIZE 256U

static bool signal_queue_push(thread_t *thread, const npk_signal_info_t *info) {
    if (!thread || !info || thread->signal_queue_count >= NPK_SIGNAL_QUEUE_MAX) return false;
    thread->signal_queue[thread->signal_queue_tail] = *info;
    thread->signal_queue_tail = (uint8_t)((thread->signal_queue_tail + 1U) % NPK_SIGNAL_QUEUE_MAX);
    ++thread->signal_queue_count;
    return true;
}

static bool signal_queue_contains(const thread_t *thread, uint64_t signal) {
    if (!thread) return false;
    for (size_t i = 0; i < thread->signal_queue_count; ++i) {
        size_t index = (thread->signal_queue_head + i) % NPK_SIGNAL_QUEUE_MAX;
        if (thread->signal_queue[index].signal == signal) return true;
    }
    return false;
}

static bool signal_queue_pop(thread_t *thread, uint64_t signal, npk_signal_info_t *info) {
    if (!thread || !info || thread->signal_queue_count == 0) return false;
    size_t found = NPK_SIGNAL_QUEUE_MAX;
    for (size_t i = 0; i < thread->signal_queue_count; ++i) {
        size_t index = (thread->signal_queue_head + i) % NPK_SIGNAL_QUEUE_MAX;
        if (thread->signal_queue[index].signal == signal) { found = i; break; }
    }
    if (found == NPK_SIGNAL_QUEUE_MAX) return false;
    size_t index = (thread->signal_queue_head + found) % NPK_SIGNAL_QUEUE_MAX;
    *info = thread->signal_queue[index];
    for (size_t i = found; i + 1 < thread->signal_queue_count; ++i) {
        size_t from = (thread->signal_queue_head + i + 1) % NPK_SIGNAL_QUEUE_MAX;
        size_t to = (thread->signal_queue_head + i) % NPK_SIGNAL_QUEUE_MAX;
        thread->signal_queue[to] = thread->signal_queue[from];
    }
    --thread->signal_queue_count;
    thread->signal_queue_tail = (uint8_t)((thread->signal_queue_head + thread->signal_queue_count) % NPK_SIGNAL_QUEUE_MAX);
    return true;
}

static bool process_deliver_signal_with_info(uint64_t *frame, uint64_t signal,
                                             const npk_signal_info_t *info) {
    thread_t *thread = current;
    process_t *process = process_current();
    if (!frame || !thread || !process || !process->alive || thread->privilege_ring != 3 ||
        signal == 0 || signal > NPK_SIGNAL_MAX || thread->signal_inflight)
        return false;

    const npk_sigaction_t *action = &process->signal_actions[signal];
    if (!signal_action_handler_valid(action)) {
        LOG_ERRORF("signal", "action rejected", signal);
        return false;
    }

    bool use_siginfo = info != NULL && (action->flags & NPK_SA_SIGINFO) != 0;
    uint64_t payload_size = use_siginfo ? NPK_USER_SIGINFO_SIZE + NPK_USER_UCONTEXT_SIZE : 0;
    uint64_t frame_size = sizeof(uint64_t) + sizeof(npk_user_signal_header_t) + payload_size;
    uint64_t old_rsp = frame[18];
    if (old_rsp < frame_size || old_rsp - frame_size < NPK_USER_MIN) return false;
    vaddr_t signal_frame_user = (old_rsp - frame_size) & ~0xFULL;
    if (!vmm_is_user_range(signal_frame_user, frame_size)) return false;

    uint64_t restorer = action->restorer;
    npk_user_signal_header_t header = {
        .magic = NPK_SIGNAL_FRAME_MAGIC,
        .signal = signal,
        .siginfo = use_siginfo ? signal_frame_user + sizeof(uint64_t) + sizeof(header) : 0,
        .ucontext = use_siginfo ? signal_frame_user + sizeof(uint64_t) + sizeof(header) + NPK_USER_SIGINFO_SIZE : 0,
    };
    if (!copyout_process(process, signal_frame_user, &restorer, sizeof(restorer)) ||
        !copyout_process(process, signal_frame_user + sizeof(restorer), &header, sizeof(header))) {
        LOG_ERRORF("signal", "frame copyout failed", signal_frame_user);
        return false;
    }
    if (use_siginfo) {
        uint8_t raw_info[NPK_USER_SIGINFO_SIZE] = {0};
        int32_t signo = (int32_t)signal;
        int32_t code = (int32_t)info->code;
        int32_t sender_pid = (int32_t)info->sender_pid;
        uint32_t sender_uid = info->sender_uid;
        memcpy(raw_info + 0, &signo, sizeof(signo));
        memcpy(raw_info + 8, &code, sizeof(code));
        memcpy(raw_info + 16, &sender_pid, sizeof(sender_pid));
        memcpy(raw_info + 20, &sender_uid, sizeof(sender_uid));
        memcpy(raw_info + 24, &info->value, sizeof(info->value));
        uint8_t context[NPK_USER_UCONTEXT_SIZE] = {0};
        if (!copyout_process(process, header.siginfo, raw_info, sizeof(raw_info)) ||
            !copyout_process(process, header.ucontext, context, sizeof(context))) return false;
    }

    memcpy(thread->signal_saved_frame, frame, sizeof(thread->signal_saved_frame));
    thread->signal_saved_mask = thread->signal_blocked;
    thread->signal_inflight = 1;
    thread->signal_frame_user = signal_frame_user;
    thread->signal_restorer = restorer;
    thread->signal_number = signal;
    thread->signal_blocked |= signal_bit(signal) | action->mask[0];

    frame[9] = signal;                 /* RDI / signo */
    frame[10] = header.siginfo;        /* RSI / siginfo_t */
    frame[11] = header.ucontext;       /* RDX / ucontext_t */
    frame[15] = action->handler;
    frame[16] = USER_CS;
    frame[17] |= 0x200ULL;
    frame[18] = signal_frame_user;
    frame[19] = USER_SS;
    return true;
}

bool process_deliver_signal(uint64_t *frame, uint64_t signal) {
    return process_deliver_signal_with_info(frame, signal, NULL);
}

uint64_t process_sigreturn(struct syscall_frame *raw_frame) {
    syscall_frame_t *frame = (syscall_frame_t *)raw_frame;
    thread_t *thread = current;
    if (!frame || !thread || !thread->signal_inflight ||
        frame->rsp != thread->signal_frame_user + sizeof(uint64_t) ||
        !vmm_is_user_range(frame->rip, 1))
        return (uint64_t)-PROCESS_EINVAL;

    npk_user_signal_header_t header;
    if (!vmm_copyin(&header, thread->signal_frame_user + sizeof(uint64_t), sizeof(header)) ||
        header.magic != NPK_SIGNAL_FRAME_MAGIC || header.signal != thread->signal_number ||
        frame->rip < thread->signal_restorer ||
        frame->rip - thread->signal_restorer > 32)
        return (uint64_t)-PROCESS_EINVAL;

    const uint64_t *saved = thread->signal_saved_frame;
    frame->r15 = saved[0];
    frame->r14 = saved[1];
    frame->r13 = saved[2];
    frame->r12 = saved[3];
    frame->r11 = saved[4];
    frame->r10 = saved[5];
    frame->r9 = saved[6];
    frame->r8 = saved[7];
    frame->rbp = saved[8];
    frame->rdi = saved[9];
    frame->rsi = saved[10];
    frame->rdx = saved[11];
    frame->rcx = saved[12];
    frame->rbx = saved[13];
    frame->rax = saved[14];
    frame->rip = saved[15];
    frame->cs = saved[16];
    frame->rflags = saved[17];
    frame->rsp = saved[18];
    frame->ss = saved[19];
    thread->signal_blocked = thread->signal_saved_mask;
    thread->signal_saved_mask = 0;
    thread->signal_inflight = 0;
    thread->signal_frame_user = 0;
    thread->signal_restorer = 0;
    thread->signal_number = 0;
    memset(thread->signal_saved_frame, 0, sizeof(thread->signal_saved_frame));
    return frame->rax;
}

static bool process_deliver_pending_common(uint64_t *frame) {
    thread_t *thread = current;
    process_t *process = process_current();
    if (!frame || !thread || !process || !process->alive || thread->privilege_ring != 3 || thread->signal_inflight)
        return false;

    uint64_t deliverable = thread->signal_pending & ~thread->signal_blocked;
    for (uint64_t signal = 1; signal <= NPK_SIGNAL_MAX; ++signal) {
        uint64_t bit = signal_bit(signal);
        if (!bit || !(deliverable & bit)) continue;
        thread->signal_pending &= ~bit;
        const npk_sigaction_t *action = &process->signal_actions[signal];
        npk_signal_info_t info;
        bool has_info = signal_queue_pop(thread, signal, &info);
        if (signal_queue_contains(thread, signal)) thread->signal_pending |= bit;
        if (action->handler == 1 && !signal_uncatchable(signal)) continue; /* SIG_IGN. */
        if (signal_uncatchable(signal) || action->handler == 0) {
            process_terminate_by_signal(signal);
        }
        if ((has_info ? process_deliver_signal_with_info(frame, signal, &info) : process_deliver_signal(frame, signal))) return true;
        /* A malformed user action or failed frame setup is fatal for a pending
         * signal rather than silently dropping an externally requested event. */
        process_terminate_by_signal(signal);
    }
    return false;
}

bool process_deliver_pending_signal(uint64_t *frame) {
    return process_deliver_pending_common(frame);
}

bool process_deliver_pending_signal_syscall(struct syscall_frame *raw_frame) {
    if (!raw_frame) return false;
    syscall_frame_t *frame = (syscall_frame_t *)raw_frame;
    uint64_t wrapper[20];
    wrapper[0] = frame->r15; wrapper[1] = frame->r14; wrapper[2] = frame->r13;
    wrapper[3] = frame->r12; wrapper[4] = frame->r11; wrapper[5] = frame->r10;
    wrapper[6] = frame->r9;  wrapper[7] = frame->r8;  wrapper[8] = frame->rbp;
    wrapper[9] = frame->rdi; wrapper[10] = frame->rsi; wrapper[11] = frame->rdx;
    wrapper[12] = frame->rcx; wrapper[13] = frame->rbx; wrapper[14] = frame->rax;
    wrapper[15] = frame->rip; wrapper[16] = frame->cs; wrapper[17] = frame->rflags;
    wrapper[18] = frame->rsp; wrapper[19] = frame->ss;
    if (!process_deliver_pending_common(wrapper)) return false;
    frame->r15 = wrapper[0]; frame->r14 = wrapper[1]; frame->r13 = wrapper[2];
    frame->r12 = wrapper[3]; frame->r11 = wrapper[4]; frame->r10 = wrapper[5];
    frame->r9 = wrapper[6]; frame->r8 = wrapper[7]; frame->rbp = wrapper[8];
    frame->rdi = wrapper[9]; frame->rsi = wrapper[10]; frame->rdx = wrapper[11];
    frame->rcx = wrapper[12]; frame->rbx = wrapper[13]; frame->rax = wrapper[14];
    frame->rip = wrapper[15]; frame->cs = wrapper[16]; frame->rflags = wrapper[17];
    frame->rsp = wrapper[18]; frame->ss = wrapper[19];
    return true;
}

__attribute__((noreturn)) void process_terminate_by_signal(uint64_t signal) {
    if (current && current != &threads[0] && current->owner && current->owner->alive) {
        uint64_t status = 128ULL + signal;
        process_exit_current((uint8_t)(status > 255 ? 255 : status));
        LOG_ERRORF("signal", "process terminated by signal", signal);
        scheduler_yield();
    }
    arch_halt();
    __builtin_unreachable();
}

static void futex_complete_thread(thread_t *thread, int64_t result) {
    if (!thread) return;
    for (unsigned i = 0; i < NPK_FUTEX_MAX_WAITERS; ++i) {
        futex_waiter_t *waiter = &futex_waiters[i];
        if (!waiter->active || waiter->thread != thread) continue;
        waiter->active = false;
        thread->futex_waiting = 0;
        thread->futex_result = result;
        if (thread->state == THREAD_BLOCKED) thread->state = THREAD_READY;
    }
}

static void futex_cancel_thread(thread_t *thread) {
    futex_complete_thread(thread, -PROCESS_EINTR);
}

static uint32_t futex_wake_internal(paddr_t root, vaddr_t user_address, uint32_t count) {
    uint32_t woken = 0;
    for (unsigned i = 0; i < NPK_FUTEX_MAX_WAITERS && woken < count; ++i) {
        futex_waiter_t *waiter = &futex_waiters[i];
        if (!waiter->active || waiter->address_space_root != root ||
            waiter->user_address != user_address) continue;
        futex_complete_thread(waiter->thread, 0);
        ++woken;
    }
    return woken;
}

static void clear_child_tid(thread_t *thread) {
    if (!thread || !thread->clear_child_tid || !thread->owner) return;
    process_t *process = thread->owner;
    vaddr_t address = thread->clear_child_tid;
    if (vmm_is_user_range(address, sizeof(uint32_t)) &&
        vm_ensure_user_page(process, address, true)) {
        uint32_t zero = 0;
        (void)copyout_process(process, address, &zero, sizeof(zero));
        (void)futex_wake_internal(process->address_space_root, address, UINT32_MAX);
    }
    thread->clear_child_tid = 0;
}

static void futex_expire_waiters(void) {
    uint64_t now = timer_ticks();
    for (unsigned i = 0; i < NPK_FUTEX_MAX_WAITERS; ++i) {
        futex_waiter_t *waiter = &futex_waiters[i];
        if (!waiter->active || !waiter->timed) continue;
        /* Signed subtraction makes the comparison safe across a uint64 tick wrap. */
        if ((int64_t)(now - waiter->deadline) >= 0)
            futex_complete_thread(waiter->thread, -PROCESS_ETIMEDOUT);
    }
}

static bool futex_timespec_to_ticks(vaddr_t user_timeout, uint64_t *ticks_out) {
    if (!ticks_out) return false;
    *ticks_out = 0;
    if (!user_timeout) return true;
    futex_timespec_t timeout;
    if (!vmm_copyin(&timeout, user_timeout, sizeof(timeout))) return false;
    if (timeout.seconds < 0 || timeout.nanoseconds < 0 || timeout.nanoseconds >= 1000000000LL)
        return false;
    uint64_t frequency = timer_frequency();
    if ((uint64_t)timeout.seconds > UINT64_MAX / frequency) return false;
    uint64_t ticks = (uint64_t)timeout.seconds * frequency;
    uint64_t fractional = ((uint64_t)timeout.nanoseconds * frequency) / 1000000000ULL;
    if (ticks > UINT64_MAX - fractional) return false;
    ticks += fractional;
    if (ticks == 0 && (timeout.seconds != 0 || timeout.nanoseconds != 0)) ticks = 1;
    *ticks_out = ticks;
    return true;
}

int64_t process_futex_wait(vaddr_t user_address, uint32_t expected, vaddr_t user_timeout) {
    process_t *process = process_current();
    if (!process || !current || current->privilege_ring != 3 ||
        (user_address & (sizeof(uint32_t) - 1U)) != 0 ||
        !vmm_is_user_range(user_address, sizeof(uint32_t))) return -PROCESS_EFAULT;

    uint64_t timeout_ticks = 0;
    if (!futex_timespec_to_ticks(user_timeout, &timeout_ticks)) return -PROCESS_EFAULT;
    uint32_t observed = 0;
    if (!vmm_copyin(&observed, user_address, sizeof(observed))) return -PROCESS_EFAULT;
    if (observed != expected) return -PROCESS_EAGAIN;
    if (user_timeout && timeout_ticks == 0) return -PROCESS_ETIMEDOUT;

    futex_waiter_t *slot = NULL;
    for (unsigned i = 0; i < NPK_FUTEX_MAX_WAITERS; ++i) {
        if (!futex_waiters[i].active) {
            slot = &futex_waiters[i];
            break;
        }
    }
    if (!slot) return -PROCESS_ENOMEM;

    slot->active = true;
    slot->thread = current;
    slot->address_space_root = process->address_space_root;
    slot->user_address = user_address;
    slot->timed = timeout_ticks != 0;
    slot->deadline = timeout_ticks ? timer_ticks() + timeout_ticks : 0;
    current->futex_waiting = 1;
    current->futex_result = -PROCESS_EINTR;
    current->state = THREAD_BLOCKED;
    scheduler_yield();

    /* If no alternate runnable thread existed, scheduler_yield() can return
     * without transferring control. Do not leave a stale waiter behind. The
     * thread-owned flag is used instead of the table slot: another waiter may
     * have legitimately reused that slot before this thread resumes. */
    if (current->futex_waiting) {
        futex_cancel_thread(current);
        current->futex_result = -PROCESS_EAGAIN;
        current->state = THREAD_RUNNING;
        return -PROCESS_EAGAIN;
    }
    int64_t result = current->futex_result;
    current->futex_waiting = 0;
    current->state = THREAD_RUNNING;
    return result;
}

int64_t process_futex_wake(vaddr_t user_address, uint32_t count) {
    process_t *process = process_current();
    if (!process || current->privilege_ring != 3 ||
        (user_address & (sizeof(uint32_t) - 1U)) != 0 ||
        !vmm_is_user_range(user_address, sizeof(uint32_t))) return -PROCESS_EFAULT;
    if (count == 0) return 0;
    return (int64_t)futex_wake_internal(process->address_space_root, user_address, count);
}

static process_t *signal_target_process(int64_t pid) {
    process_t *sender = process_current();
    if (pid == 0) return sender;
    if (pid > 0) return find_process_by_pid((uint64_t)pid);
    return NULL; /* Process groups are deliberately not exposed yet. */
}

static thread_t *select_signal_thread(process_t *process, uint64_t bit) {
    thread_t *fallback = NULL;
    if (!process) return NULL;
    for (unsigned i = 0; i < NPK_MAX_THREADS; ++i) {
        thread_t *thread = &threads[i];
        if (thread->owner != process || thread->state == THREAD_UNUSED ||
            thread->state == THREAD_ZOMBIE) continue;
        if (!fallback) fallback = thread;
        if ((thread->signal_blocked & bit) == 0) return thread;
    }
    return fallback;
}

static int64_t queue_signal_to_thread(thread_t *target, uint64_t signal,
                                       const npk_signal_info_t *info) {
    uint64_t bit = signal_bit(signal);
    if (!target || !bit) return -PROCESS_ESRCH;
    if (info && !signal_queue_push(target, info)) return -PROCESS_EAGAIN;
    target->signal_pending |= bit;
    if (target->futex_waiting &&
        (signal_uncatchable(signal) || !(target->signal_blocked & bit)))
        futex_complete_thread(target, -PROCESS_EINTR);
    return 0;
}

int64_t process_send_signal_info(int64_t pid, uint64_t signal, const npk_signal_info_t *info) {
    if (signal > NPK_SIGNAL_MAX) return -PROCESS_EINVAL;
    process_t *target = signal_target_process(pid);
    if (!target || !target->alive) return -PROCESS_ESRCH;
    if (signal == 0) return 0;
    uint64_t bit = signal_bit(signal);
    thread_t *recipient = select_signal_thread(target, bit);
    if (!recipient) return -PROCESS_ESRCH;
    return queue_signal_to_thread(recipient, signal, info);
}

int64_t process_send_signal(int64_t pid, uint64_t signal) {
    return process_send_signal_info(pid, signal, NULL);
}

int64_t process_send_thread_signal_info(int64_t tgid, int64_t tid, uint64_t signal,
                                        const npk_signal_info_t *info) {
    if (tgid <= 0 || tid <= 0 || signal > NPK_SIGNAL_MAX) return -PROCESS_EINVAL;
    process_t *target_process = find_process_by_pid((uint64_t)tgid);
    if (!target_process || !target_process->alive) return -PROCESS_ESRCH;
    for (unsigned i = 0; i < NPK_MAX_THREADS; ++i) {
        thread_t *target = &threads[i];
        if (target->tid == (uint64_t)tid && target->owner == target_process && target->state != THREAD_UNUSED) {
            if (signal == 0) return 0;
            return queue_signal_to_thread(target, signal, info);
        }
    }
    return -PROCESS_ESRCH;
}

int64_t process_send_thread_signal(int64_t tgid, int64_t tid, uint64_t signal) {
    return process_send_thread_signal_info(tgid, tid, signal, NULL);
}

uint64_t process_get_signal_mask(void) {
    return current ? current->signal_blocked : 0;
}

void process_set_signal_mask(uint64_t mask) {
    if (!current) return;
    current->signal_blocked = mask & ~signal_bit(9) & ~signal_bit(19);
}

static void discard_incoming_fd_transfers(process_t *process) {
    if (!process) return;
    for (uint32_t i = 0; i < process->incoming_fd_count; ++i)
        if (process->incoming_fd_transfers[i].handle >= 0)
            (void)vfs_close(process->incoming_fd_transfers[i].handle);
    memset(process->incoming_fd_transfers, 0, sizeof(process->incoming_fd_transfers));
    process->incoming_fd_count = 0;
}

static void close_process_fds(process_t *process) {
    if (!process) return;
    for (unsigned i = 0; i < NPK_MAX_FDS; ++i) {
        if (process->fds[i] >= 0) {
            (void)vfs_close(process->fds[i]);
            process->fds[i] = -1;
        }
    }
}

static void destroy_process_memory(process_t *process) {
    if (!process) return;
    paddr_t root = process->address_space_root;
    vm_destroy_all(process);
    if (!root) return;
    if (root == vmm_current_root()) {
        if (kernel_address_space_root && kernel_address_space_root != root)
            __asm__ volatile ("mov %0, %%cr3" : : "r"(kernel_address_space_root) : "memory");
    }
    if (root != kernel_address_space_root && root != vmm_current_root())
        vmm_destroy_address_space(root);
    process->address_space_root = 0;
}

static void reap_process(process_t *process) {
    if (!process || process == kernel_process || process->alive || process->pid == 0) return;
    if (current && current->owner == process) return;
    destroy_process_memory(process);
    close_process_fds(process);
    for (unsigned i = 1; i < NPK_MAX_THREADS; ++i) {
        thread_t *thread = &threads[i];
        if (thread->owner != process) continue;
        if (thread != current && thread->kernel_stack) kfree((void *)thread->kernel_stack);
        memset(thread, 0, sizeof(*thread));
    }
    memset(process, 0, sizeof(*process));
}

static void wake_parent_waiter(process_t *child) {
    if (!child || child->parent_pid == 0) return;
    process_t *parent = find_process_by_pid(child->parent_pid);
    if (!parent || !parent->alive || !parent->wait_active ||
        !wait_target_matches(parent->wait_target, child)) return;
    uint32_t status = (uint32_t)child->exit_status << 8;
    bool status_ok = parent->wait_status_user == 0 ||
                     copyout_process(parent, parent->wait_status_user, &status, sizeof(status));
    thread_t *waiter = NULL;
    for (unsigned i = 0; i < NPK_MAX_THREADS; ++i) {
        thread_t *candidate = &threads[i];
        if (candidate->owner == parent && candidate->state == THREAD_BLOCKED) {
            waiter = candidate;
            break;
        }
    }
    if (!waiter) return;
    parent->wait_active = 0;
    parent->wait_result = status_ok ? (int64_t)child->pid : -PROCESS_EFAULT;
    parent->wait_woken = 1;
    if (status_ok) child->waited = 1;
    waiter->state = THREAD_READY;
    if (waiter->saved_interrupt_frame)
        waiter->saved_interrupt_frame[0] = (uint64_t)parent->wait_result;
}

static thread_t *allocate_thread(void) {
    for (unsigned i = 1; i < NPK_MAX_THREADS; ++i) {
        if (threads[i].state != THREAD_UNUSED) continue;
        memset(&threads[i], 0, sizeof(threads[i]));
        threads[i].tid = next_tid++;
        threads[i].state = THREAD_READY;
        threads[i].thread_exit_status = 0;
        return &threads[i];
    }
    return NULL;
}

static void scheduler_thread_bootstrap(void) {
    thread_t *thread = current;
    if (thread && thread->entry) thread->entry(thread->argument);
    if (thread) thread->state = THREAD_ZOMBIE;
    for (;;) scheduler_yield();
}

static void activate_thread(thread_t *thread) {
    if (!thread || !thread->owner) return;
    current = thread;
    thread->state = THREAD_RUNNING;
    if (thread->context.cr3 && thread->context.cr3 != vmm_current_root())
        __asm__ volatile ("mov %0, %%cr3" : : "r"(thread->context.cr3) : "memory");
    if (thread->kernel_stack_top) tss_set_rsp0(thread->kernel_stack_top);
    arch_set_fs_base(thread->privilege_ring == 3 ? thread->fs_base : 0);
    arch_set_user_gs_base(thread->privilege_ring == 3 ? thread->gs_base : 0);
    syscall_set_current_process(thread->owner);
}

static thread_t *next_runnable(thread_t *from) {
    unsigned start = 0;
    if (from >= threads && from < threads + NPK_MAX_THREADS)
        start = (unsigned)(from - threads);
    for (unsigned offset = 1; offset <= NPK_MAX_THREADS; ++offset) {
        unsigned index = (start + offset) % NPK_MAX_THREADS;
        thread_t *candidate = &threads[index];
        if (candidate == &threads[0] && candidate != from) continue;
        if (candidate->state == THREAD_READY && candidate->owner && candidate->owner->alive)
            return candidate;
    }
    return NULL;
}

void process_init(void) {
    memset(processes, 0, sizeof(processes));
    memset(threads, 0, sizeof(threads));
    kernel_process = &processes[0];
    kernel_process->pid = next_pid++;
    kernel_process->thread_count = 1;
    kernel_process->alive = 1;
    kernel_address_space_root = vmm_current_root();
    for (unsigned i = 0; i < NPK_MAX_FDS; ++i) kernel_process->fds[i] = -1;
    kernel_process->fds[0] = 0; kernel_process->fds[1] = 1; kernel_process->fds[2] = 2;
    memset(kernel_process->fd_flags, 0, sizeof(kernel_process->fd_flags));
    thread_t *thread = &threads[0];
    thread->tid = next_tid++;
    thread->state = THREAD_RUNNING;
    thread->privilege_ring = 0;
    thread->owner = kernel_process;
    kernel_process->main_thread = thread;
    thread->group_leader = 1;
    current = thread;
    syscall_set_current_process(kernel_process);
    LOG_INFOF("proc", "kernel pid", kernel_process->pid);
    LOG_INFOF("proc", "kernel tid", thread->tid);
}

process_t *process_create_user(paddr_t address_space_root, uint64_t entry, uint64_t stack_top) {
    if (!address_space_root) {
        log_message(LOG_ERROR, "proc", "create_user rejected null address-space root");
        return NULL;
    }
    if (!vmm_is_user_range(entry, 1)) {
        LOG_ERRORF("proc", "create_user rejected entry", entry);
        return NULL;
    }
    if (stack_top < NPK_PAGE_SIZE || !vmm_is_user_range(stack_top - NPK_PAGE_SIZE, NPK_PAGE_SIZE)) {
        LOG_ERRORF("proc", "create_user rejected user stack", stack_top);
        return NULL;
    }
    for (unsigned i = 1; i < NPK_MAX_PROCESSES; ++i) {
        if (processes[i].alive) continue;
        thread_t *thread = allocate_thread();
        if (!thread) return NULL;
        process_t *process = &processes[i];
        memset(process, 0, sizeof(*process));
        for (unsigned i = 0; i < NPK_MAX_FDS; ++i) process->fds[i] = -1;
        process->fds[0] = 0; process->fds[1] = 1; process->fds[2] = 2;
        memset(process->fd_flags, 0, sizeof(process->fd_flags));
        process->pid = next_pid++;
        process->thread_count = 1;
        process->alive = 1;
        process->address_space_root = address_space_root;
        process->main_thread = thread;
        thread->group_leader = 1;
        thread->privilege_ring = 3;
        thread->owner = process;
        thread->context.rip = entry;
        thread->context.rsp = stack_top;
        thread->context.rflags = 0x202;
        thread->context.cr3 = address_space_root;
        process->user_stack_top = NPK_USER_STACK_TOP;
        process->user_stack_limit = NPK_USER_STACK_TOP - (uint64_t)NPK_USER_STACK_PAGES * NPK_PAGE_SIZE;
        process->user_stack_bottom = stack_top & ~(NPK_PAGE_SIZE - 1);
        thread->user_stack_top = process->user_stack_top;
        thread->kernel_stack = (uint64_t)kcalloc(1, NPK_KERNEL_STACK_SIZE);
        if (!thread->kernel_stack) {
            log_message(LOG_ERROR, "proc", "create_user kernel stack allocation failed");
            thread->state = THREAD_ZOMBIE;
            process->alive = 0;
            return NULL;
        }
            thread->kernel_stack_top = (thread->kernel_stack + NPK_KERNEL_STACK_SIZE) & ~0xFULL;
        prepare_user_entry_frame(thread, entry, stack_top);
        LOG_INFOF("proc", "created user pid", process->pid);
        return process;
    }
    return NULL;
}

static bool user_stack_write(process_t *process, paddr_t root, vaddr_t address, const void *source, size_t length) {
    if (!process || !vmm_is_user_range(address, length)) return false;
    const uint8_t *bytes = (const uint8_t *)source;
    size_t written = 0;
    while (written < length) {
        vaddr_t current_address = address + written;
        paddr_t physical = vmm_lookup_root(root, current_address, VM_USER | VM_WRITE);
        if (!physical) {
            if (!vm_ensure_user_page(process, current_address, true)) return false;
            physical = vmm_lookup_root(root, current_address, VM_USER | VM_WRITE);
        }
        if (!physical) return false;
        size_t page_left = NPK_PAGE_SIZE - (size_t)(current_address & (NPK_PAGE_SIZE - 1));
        size_t amount = length - written < page_left ? length - written : page_left;
        memcpy((uint8_t *)phys_to_virt(physical), bytes + written, amount);
        written += amount;
    }
    return true;
}

static bool user_stack_push(process_t *process, paddr_t root, uint64_t *stack_pointer, uint64_t value) {
    if (*stack_pointer < NPK_USER_MIN + sizeof(value)) return false;
    *stack_pointer -= sizeof(value);
    return user_stack_write(process, root, *stack_pointer, &value, sizeof(value));
}

typedef struct {
    uint64_t type;
    uint64_t value;
} exec_auxv_pair_t;

#define NPK_AT_NULL 0ULL
#define NPK_AT_PHDR 3ULL
#define NPK_AT_PHENT 4ULL
#define NPK_AT_PHNUM 5ULL
#define NPK_AT_PAGESZ 6ULL
#define NPK_AT_BASE 7ULL
#define NPK_AT_ENTRY 9ULL

static bool build_initial_user_stack(process_t *process, paddr_t root, uint64_t stack_top,
                                     const char *const *argv, size_t argc,
                                     const char *const *envp, size_t envc,
                                     const elf_load_result_t *main_image,
                                     const elf_load_result_t *interpreter,
                                     uint64_t *initial_stack) {
    if (argc > 64 || envc > 64 || initial_stack == NULL || !main_image) return false;
    uint64_t argv_addresses[64];
    uint64_t env_addresses[64];
    uint64_t stack_pointer = stack_top;
    for (size_t i = argc; i > 0; --i) {
        const char *string = argv[i - 1];
        if (string == NULL) return false;
        size_t length = strlen(string) + 1;
        if (length > NPK_PAGE_SIZE || stack_pointer < NPK_USER_MIN + length) return false;
        stack_pointer -= length;
        if (!user_stack_write(process, root, stack_pointer, string, length)) return false;
        argv_addresses[i - 1] = stack_pointer;
    }
    for (size_t i = envc; i > 0; --i) {
        const char *string = envp[i - 1];
        if (string == NULL) return false;
        size_t length = strlen(string) + 1;
        if (length > NPK_PAGE_SIZE || stack_pointer < NPK_USER_MIN + length) return false;
        stack_pointer -= length;
        if (!user_stack_write(process, root, stack_pointer, string, length)) return false;
        env_addresses[i - 1] = stack_pointer;
    }
    stack_pointer &= ~0xFULL;

    exec_auxv_pair_t auxv[8];
    size_t auxc = 0;
    if (main_image->program_header != 0) {
        auxv[auxc++] = (exec_auxv_pair_t){NPK_AT_PHDR, main_image->program_header};
        auxv[auxc++] = (exec_auxv_pair_t){NPK_AT_PHENT, main_image->program_header_size};
        auxv[auxc++] = (exec_auxv_pair_t){NPK_AT_PHNUM, main_image->program_header_count};
    }
    auxv[auxc++] = (exec_auxv_pair_t){NPK_AT_PAGESZ, NPK_PAGE_SIZE};
    if (interpreter && interpreter->has_interp != 0 && interpreter->entry != 0)
        auxv[auxc++] = (exec_auxv_pair_t){NPK_AT_BASE, interpreter->load_bias};
    auxv[auxc++] = (exec_auxv_pair_t){NPK_AT_ENTRY, main_image->entry};
    auxv[auxc++] = (exec_auxv_pair_t){NPK_AT_NULL, NPK_AT_NULL};
    for (size_t i = auxc; i > 0; --i) {
        if (!user_stack_push(process, root, &stack_pointer, auxv[i - 1].value) ||
            !user_stack_push(process, root, &stack_pointer, auxv[i - 1].type)) return false;
    }
    if (!user_stack_push(process, root, &stack_pointer, 0)) return false;
    for (size_t i = envc; i > 0; --i)
        if (!user_stack_push(process, root, &stack_pointer, env_addresses[i - 1])) return false;
    if (!user_stack_push(process, root, &stack_pointer, 0)) return false;
    for (size_t i = argc; i > 0; --i)
        if (!user_stack_push(process, root, &stack_pointer, argv_addresses[i - 1])) return false;
    if (!user_stack_push(process, root, &stack_pointer, argc)) return false;
    *initial_stack = stack_pointer;
    return true;
}

typedef struct {
    process_t process;
    uint64_t entry;
    uint64_t initial_stack;
    elf_load_result_t main_image;
    elf_load_result_t interpreter_image;
    uint8_t has_interpreter;
} staged_exec_t;

static bool register_exec_vmas(process_t *image_process, const void *image,
                               size_t image_size, const elf_load_result_t *loaded) {
    if (!image_process || !image || !loaded || image_size < sizeof(elf64_header_t)) return false;
    const elf64_header_t *header = (const elf64_header_t *)image;
    const uint8_t *bytes = (const uint8_t *)image;
    if (header->phoff > image_size || header->phentsize != sizeof(elf64_program_header_t) ||
        (uint64_t)header->phnum * header->phentsize > image_size - header->phoff)
        return false;
    const elf64_program_header_t *programs =
        (const elf64_program_header_t *)(bytes + header->phoff);
    for (uint16_t i = 0; i < header->phnum; ++i) {
        const elf64_program_header_t *ph = &programs[i];
        if (ph->type != PT_LOAD || ph->memsz == 0) continue;
        if (ph->vaddr > UINT64_MAX - loaded->load_bias ||
            ph->memsz > UINT64_MAX - (ph->vaddr + loaded->load_bias)) return false;
        uint64_t start = (ph->vaddr + loaded->load_bias) & ~(NPK_PAGE_SIZE - 1);
        uint64_t raw_end = ph->vaddr + loaded->load_bias + ph->memsz;
        if (raw_end > UINT64_MAX - (NPK_PAGE_SIZE - 1)) return false;
        uint64_t end = (raw_end + NPK_PAGE_SIZE - 1) & ~(NPK_PAGE_SIZE - 1);
        uint64_t protection = 0;
        if (ph->flags & PF_R) protection |= NPK_PROT_READ;
        if (ph->flags & PF_W) protection |= NPK_PROT_WRITE;
        if (ph->flags & PF_X) protection |= NPK_PROT_EXEC;
        if (end <= start || vm_reserve_range(image_process, start, end - start,
                                             protection, NPK_MAP_PRIVATE | NPK_MAP_ANONYMOUS) < 0)
            return false;
    }
    return true;
}

#define NPK_MAX_EXEC_IMAGE (16U * 1024U * 1024U)

static bool read_vfs_image(const char *path, uint8_t **image, size_t *image_size) {
    if (!path || !image || !image_size) return false;
    *image = NULL;
    *image_size = 0;
    int fd = vfs_open(path);
    if (fd < 0) return false;
    ssize_t raw_size = vfs_size(fd);
    if (raw_size <= 0 || (uint64_t)raw_size > NPK_MAX_EXEC_IMAGE) {
        (void)vfs_close(fd);
        return false;
    }
    uint8_t *buffer = (uint8_t *)kcalloc(1, (size_t)raw_size);
    if (!buffer) {
        (void)vfs_close(fd);
        return false;
    }
    size_t offset = 0;
    while (offset < (size_t)raw_size) {
        ssize_t amount = vfs_read(fd, buffer + offset, (size_t)raw_size - offset);
        if (amount <= 0 || (size_t)amount > (size_t)raw_size - offset) {
            kfree(buffer);
            (void)vfs_close(fd);
            return false;
        }
        offset += (size_t)amount;
    }
    (void)vfs_close(fd);
    *image = buffer;
    *image_size = (size_t)raw_size;
    return true;
}

static bool stage_exec_image(const void *image, size_t image_size,
                             const char *const *argv, size_t argc,
                             const char *const *envp, size_t envc,
                             staged_exec_t *staged) {
    if (!staged || !elf64_validate(image, image_size)) return false;
    memset(staged, 0, sizeof(*staged));
    const elf64_header_t *header = (const elf64_header_t *)image;
    uint64_t bias = header->type == ET_DYN ? NPK_USER_DYN_BASE : 0;
    char interpreter_path[NPK_ELF_INTERP_MAX];
    int interpreter_length = elf64_get_interpreter(image, image_size,
                                                    interpreter_path, sizeof(interpreter_path));
    if (interpreter_length < 0) return false;
    uint8_t *interpreter_bytes = NULL;
    size_t interpreter_size = 0;
    if (interpreter_length > 0 &&
        !read_vfs_image(interpreter_path, &interpreter_bytes, &interpreter_size)) return false;
    if (interpreter_bytes) {
        const elf64_header_t *interpreter_header = (const elf64_header_t *)interpreter_bytes;
        char nested_interpreter[NPK_ELF_INTERP_MAX];
        int nested_length = elf64_get_interpreter(interpreter_bytes, interpreter_size,
                                                  nested_interpreter, sizeof(nested_interpreter));
        /* The kernel performs one interpreter handoff only. A PT_INTERP inside
         * ld.so would otherwise recurse without a defined auxv/rollback contract. */
        if (nested_length != 0 || interpreter_header->type != ET_DYN ||
            !elf64_validate(interpreter_bytes, interpreter_size)) {
            kfree(interpreter_bytes);
            return false;
        }
    }

    paddr_t root = vmm_create_address_space();
    if (!root) {
        if (interpreter_bytes) kfree(interpreter_bytes);
        return false;
    }

    staged->process.alive = 1;
    staged->process.address_space_root = root;
    if (elf64_load_in_address_space(image, image_size, root, bias, &staged->main_image) != 0 ||
        !register_exec_vmas(&staged->process, image, image_size, &staged->main_image))
        goto fail;

    if (interpreter_bytes) {
        if (elf64_load_in_address_space(interpreter_bytes, interpreter_size, root,
                                        NPK_USER_INTERP_BASE, &staged->interpreter_image) != 0 ||
            !register_exec_vmas(&staged->process, interpreter_bytes, interpreter_size,
                                &staged->interpreter_image)) goto fail;
        /* Mark the loaded interpreter as present for AT_BASE generation. The
         * interpreter itself must not recursively trigger another load. */
        staged->interpreter_image.has_interp = 1;
        staged->has_interpreter = 1;
    }

    staged->process.image_base = staged->main_image.image_base;
    staged->process.image_end = staged->main_image.image_end;
    if (staged->has_interpreter && staged->interpreter_image.image_end > staged->process.image_end)
        staged->process.image_end = staged->interpreter_image.image_end;
    if (staged->process.image_end > UINT64_MAX - (NPK_PAGE_SIZE - 1)) goto fail;
    uint64_t heap_start = (staged->process.image_end + NPK_PAGE_SIZE - 1) & ~(NPK_PAGE_SIZE - 1);
    uint64_t heap_limit = heap_start + (1ULL << 32);
    if (heap_limit < heap_start || heap_limit > NPK_USER_MAX)
        heap_limit = NPK_USER_MAX - NPK_PAGE_SIZE;
    if (vm_reserve_heap(&staged->process, heap_start, heap_limit) < 0) goto fail;

    staged->process.user_stack_top = NPK_USER_STACK_TOP;
    staged->process.user_stack_limit = NPK_USER_STACK_TOP -
        (uint64_t)NPK_USER_STACK_PAGES * NPK_PAGE_SIZE;
    staged->process.user_stack_bottom = NPK_USER_STACK_TOP;
    if (!build_initial_user_stack(&staged->process, root, NPK_USER_STACK_TOP,
                                  argv, argc, envp, envc, &staged->main_image,
                                  staged->has_interpreter ? &staged->interpreter_image : NULL,
                                  &staged->initial_stack)) goto fail;
    staged->process.brk_start = heap_start;
    staged->process.brk_end = heap_start;
    staged->process.mmap_cursor = 0x0000001000000000ULL;
    staged->entry = staged->has_interpreter ? staged->interpreter_image.entry : staged->main_image.entry;
    if (interpreter_bytes) kfree(interpreter_bytes);
    return true;

fail:
    if (interpreter_bytes) kfree(interpreter_bytes);
    vm_destroy_all(&staged->process);
    vmm_destroy_address_space(root);
    memset(staged, 0, sizeof(*staged));
    return false;
}

process_t *process_exec_image_with_args(const void *image, size_t image_size,
                                        const char *const *argv, size_t argc,
                                        const char *const *envp, size_t envc) {
    staged_exec_t staged;
    if (!stage_exec_image(image, image_size, argv, argc, envp, envc, &staged)) return NULL;
    process_t *process = process_create_user(staged.process.address_space_root,
                                             staged.entry, staged.initial_stack);
    if (!process) {
        vm_destroy_all(&staged.process);
        vmm_destroy_address_space(staged.process.address_space_root);
        return NULL;
    }
    process->image_base = staged.process.image_base;
    process->image_end = staged.process.image_end;
    process->brk_start = staged.process.brk_start;
    process->brk_end = staged.process.brk_end;
    process->mmap_cursor = staged.process.mmap_cursor;
    process->user_stack_top = staged.process.user_stack_top;
    process->user_stack_bottom = staged.process.user_stack_bottom;
    process->user_stack_limit = staged.process.user_stack_limit;
    process->vmas = staged.process.vmas;
    staged.process.vmas = NULL;
    return process;
}

int64_t process_exec_current(const void *image, size_t image_size,
                             const char *const *argv, size_t argc,
                             const char *const *envp, size_t envc,
                             struct syscall_frame *raw_frame) {
    process_t *process = process_current();
    thread_t *thread = current;
    syscall_frame_t *frame = (syscall_frame_t *)raw_frame;
    if (!process || !thread || thread->privilege_ring != 3 || !frame) return -PROCESS_EFAULT;

    staged_exec_t staged;
    if (!stage_exec_image(image, image_size, argv, argc, envp, envc, &staged)) return -PROCESS_EINVAL;

    paddr_t old_root = process->address_space_root;
    struct npk_vma *old_vmas = process->vmas;
    process_t old_image = *process;

    for (unsigned i = 0; i < NPK_MAX_FDS; ++i) {
        if ((process->fd_flags[i] & 1U) != 0 && process->fds[i] >= 0) {
            (void)vfs_close(process->fds[i]);
            process->fds[i] = -1;
            process->fd_flags[i] = 0;
        }
    }

    process->address_space_root = staged.process.address_space_root;
    process->image_base = staged.process.image_base;
    process->image_end = staged.process.image_end;
    process->mmap_cursor = staged.process.mmap_cursor;
    process->brk_start = staged.process.brk_start;
    process->brk_end = staged.process.brk_end;
    process->user_stack_top = staged.process.user_stack_top;
    process->user_stack_bottom = staged.process.user_stack_bottom;
    process->user_stack_limit = staged.process.user_stack_limit;
    process->vmas = staged.process.vmas;
    staged.process.vmas = NULL;

    memset(process->signal_actions, 0, sizeof(process->signal_actions));
    thread->signal_inflight = 0;
    thread->signal_frame_user = 0;
    thread->signal_restorer = 0;
    thread->signal_number = 0;
    thread->signal_blocked = 0;
    thread->signal_pending = 0;
    thread->signal_saved_mask = 0;
    memset(thread->signal_saved_frame, 0, sizeof(thread->signal_saved_frame));
    thread->fs_base = 0;
    thread->gs_base = 0;
    thread->context.cr3 = staged.process.address_space_root;
    thread->context.rip = staged.entry;
    thread->context.rsp = staged.initial_stack;
    thread->context.rflags = 0x202;

    frame->rip = staged.entry;
    frame->cs = USER_CS;
    frame->rflags = 0x202;
    frame->rsp = staged.initial_stack;
    frame->ss = USER_SS;
    frame->rax = 0;
    frame->rbx = frame->rcx = frame->rdx = frame->rsi = 0;
    frame->rdi = frame->rbp = frame->r8 = frame->r9 = 0;
    frame->r10 = frame->r11 = frame->r12 = frame->r13 = frame->r14 = frame->r15 = 0;

    process_activate(process);
    old_image.address_space_root = old_root;
    old_image.vmas = old_vmas;
    if (old_root && old_root != process->address_space_root) destroy_process_memory(&old_image);
    return 0;
}

process_t *process_exec_image(const void *image, size_t image_size) {
    return process_exec_image_with_args(image, image_size, NULL, 0, NULL, 0);
}

int64_t process_fork(const struct syscall_frame *parent_frame) {
    const syscall_frame_t *frame = (const syscall_frame_t *)parent_frame;
    process_t *parent = process_current();
    if (!frame || !parent || !parent->alive || !current || current->privilege_ring != 3) return -22;

    unsigned process_index = NPK_MAX_PROCESSES;
    for (unsigned i = 1; i < NPK_MAX_PROCESSES; ++i) {
        if (processes[i].pid == 0) {
            process_index = i;
            break;
        }
    }
    if (process_index == NPK_MAX_PROCESSES) return -11;

    paddr_t child_root = vmm_clone_user_space(parent->address_space_root);
    if (!child_root) return -12;
    thread_t *child_thread = allocate_thread();
    if (!child_thread) {
        vmm_destroy_address_space(child_root);
        return -11;
    }

    process_t *child = &processes[process_index];
    memset(child, 0, sizeof(*child));
    child->pid = next_pid++;
    child->parent_pid = parent->pid;
    child->thread_count = 1;
    child->alive = 1;
    child->address_space_root = child_root;
    child->image_base = parent->image_base;
    child->image_end = parent->image_end;
    child->mmap_cursor = parent->mmap_cursor;
    child->brk_start = parent->brk_start;
    child->brk_end = parent->brk_end;
    child->user_stack_top = parent->user_stack_top;
    child->user_stack_bottom = parent->user_stack_bottom;
    child->user_stack_limit = parent->user_stack_limit;
    memcpy(child->image_path, parent->image_path, sizeof(child->image_path));
    memcpy(child->fds, parent->fds, sizeof(child->fds));
    memcpy(child->fd_flags, parent->fd_flags, sizeof(child->fd_flags));
    unsigned retained_fds = 0;
    for (unsigned i = 0; i < NPK_MAX_FDS; ++i) {
        if (child->fds[i] < 0) continue;
        if (vfs_retain(child->fds[i]) != 0) {
            for (unsigned j = 0; j < i; ++j)
                if (child->fds[j] >= 0) (void)vfs_close(child->fds[j]);
            vmm_destroy_address_space(child_root);
            memset(child, 0, sizeof(*child));
            memset(child_thread, 0, sizeof(*child_thread));
            return -24;
        }
        ++retained_fds;
    }
    (void)retained_fds;
    memcpy(child->signal_actions, parent->signal_actions, sizeof(child->signal_actions));
    if (!vm_clone_metadata(parent, child)) {
        for (unsigned i = 0; i < NPK_MAX_FDS; ++i)
            if (child->fds[i] >= 0) (void)vfs_close(child->fds[i]);
        vmm_destroy_address_space(child_root);
        memset(child, 0, sizeof(*child));
        memset(child_thread, 0, sizeof(*child_thread));
        return -12;
    }

    child->main_thread = child_thread;
    child_thread->privilege_ring = 3;
    child_thread->group_leader = 1;
    child_thread->owner = child;
    child_thread->context.rip = frame->rip;
    child_thread->context.rsp = frame->rsp;
    child_thread->context.rflags = frame->rflags;
    child_thread->context.cr3 = child_root;
    child_thread->fs_base = parent->main_thread ? parent->main_thread->fs_base : 0;
    child_thread->gs_base = parent->main_thread ? parent->main_thread->gs_base : 0;
    child_thread->signal_blocked = parent->main_thread ? parent->main_thread->signal_blocked : 0;
    child_thread->signal_pending = 0;
    child_thread->signal_inflight = 0;
    child_thread->user_stack_top = child->user_stack_top;
    child_thread->kernel_stack = (uint64_t)kcalloc(1, NPK_KERNEL_STACK_SIZE);
    if (!child_thread->kernel_stack) {
        vm_destroy_all(child);
        vmm_destroy_address_space(child_root);
        memset(child, 0, sizeof(*child));
        memset(child_thread, 0, sizeof(*child_thread));
        return -12;
    }
    child_thread->kernel_stack_top = (child_thread->kernel_stack + NPK_KERNEL_STACK_SIZE) & ~0xFULL;
    if (!prepare_fork_frame(child_thread, frame)) {
        kfree((void *)child_thread->kernel_stack);
        vm_destroy_all(child);
        vmm_destroy_address_space(child_root);
        memset(child, 0, sizeof(*child));
        memset(child_thread, 0, sizeof(*child_thread));
        return -12;
    }
    LOG_INFOF("proc", "fork child pid", child->pid);
    return (int64_t)child->pid;
}

int64_t process_clone(uint64_t flags, vaddr_t child_stack, vaddr_t parent_tid,
                      vaddr_t tls, vaddr_t child_tid, const struct syscall_frame *parent_frame) {
    process_t *process = process_current();
    thread_t *parent_thread = current;
    const syscall_frame_t *frame = (const syscall_frame_t *)parent_frame;
    if (!process || !parent_thread || !frame || !process->alive ||
        parent_thread->privilege_ring != 3) return -PROCESS_EPERM;
    if ((flags & NPK_CLONE_THREAD_FLAGS) != NPK_CLONE_THREAD_FLAGS ||
        (flags & ~(NPK_CLONE_THREAD_FLAGS | NPK_CLONE_OPTIONAL_FLAGS)) != 0)
        return -PROCESS_ENOSYS;
    if (!child_stack || (child_stack & 0xFULL) != 0 ||
        child_stack < NPK_PAGE_SIZE || !vmm_is_user_range(child_stack - 16, 16))
        return -PROCESS_EFAULT;
    if ((flags & NPK_CLONE_PARENT_SETTID) != 0 &&
        (!parent_tid || !vmm_is_user_range(parent_tid, sizeof(uint32_t))))
        return -PROCESS_EFAULT;
    if ((flags & (NPK_CLONE_CHILD_SETTID | NPK_CLONE_CHILD_CLEARTID)) != 0 &&
        (!child_tid || !vmm_is_user_range(child_tid, sizeof(uint32_t))))
        return -PROCESS_EFAULT;
    if ((flags & NPK_CLONE_SETTLS) != 0 && tls && !vmm_is_user_range(tls, 1))
        return -PROCESS_EFAULT;
    if (!vm_ensure_user_page(process, child_stack - 1, true)) return -PROCESS_EFAULT;
    thread_t *child = allocate_thread();
    if (!child) return -PROCESS_EAGAIN;
    child->clone_thread = 1;
    child->group_leader = 0;
    child->owner = process;
    child->privilege_ring = 3;
    child->user_stack_top = child_stack;
    child->context.cr3 = process->address_space_root;
    child->context.rip = frame->rip;
    child->context.rsp = child_stack;
    child->context.rflags = frame->rflags;
    child->fs_base = (flags & NPK_CLONE_SETTLS) ? tls : parent_thread->fs_base;
    child->gs_base = parent_thread->gs_base;
    child->signal_blocked = parent_thread->signal_blocked;
    child->signal_pending = 0;
    child->signal_inflight = 0;
    child->kernel_stack = (uint64_t)kcalloc(1, NPK_KERNEL_STACK_SIZE);
    if (!child->kernel_stack) {
        memset(child, 0, sizeof(*child));
        return -PROCESS_ENOMEM;
    }
    child->kernel_stack_top = (child->kernel_stack + NPK_KERNEL_STACK_SIZE) & ~0xFULL;
    if (!prepare_fork_frame_with_stack(child, frame, child_stack)) {
        kfree((void *)child->kernel_stack);
        memset(child, 0, sizeof(*child));
        return -PROCESS_ENOMEM;
    }
    if ((flags & NPK_CLONE_CHILD_CLEARTID) != 0) child->clear_child_tid = child_tid;
    if ((flags & NPK_CLONE_PARENT_SETTID) != 0) {
        uint32_t tid = (uint32_t)child->tid;
        if (!copyout_process(process, parent_tid, &tid, sizeof(tid))) {
            kfree((void *)child->kernel_stack);
            memset(child, 0, sizeof(*child));
            return -PROCESS_EFAULT;
        }
    }
    if ((flags & NPK_CLONE_CHILD_SETTID) != 0) {
        uint32_t tid = (uint32_t)child->tid;
        if (!copyout_process(process, child_tid, &tid, sizeof(tid))) {
            kfree((void *)child->kernel_stack);
            memset(child, 0, sizeof(*child));
            return -PROCESS_EFAULT;
        }
    }
    if (process->thread_count == UINT32_MAX) {
        clear_child_tid(child);
        kfree((void *)child->kernel_stack);
        memset(child, 0, sizeof(*child));
        return -PROCESS_EAGAIN;
    }
    ++process->thread_count;
    return (int64_t)child->tid;
}

void process_activate(process_t *process) {
    if (!process || !process->alive || !process->main_thread) return;
    current = process->main_thread;
    current->state = THREAD_RUNNING;
    if (process->address_space_root && process->address_space_root != vmm_current_root())
        __asm__ volatile ("mov %0, %%cr3" : : "r"(process->address_space_root) : "memory");
    tss_set_rsp0(current->kernel_stack_top);
    arch_set_fs_base(current->privilege_ring == 3 ? current->fs_base : 0);
    arch_set_user_gs_base(current->privilege_ring == 3 ? current->gs_base : 0);
    syscall_set_current_process(process);
}

void process_exit_current(uint8_t status) {
    if (!current || current == &threads[0] || !current->owner || !current->owner->alive) return;
    process_t *process = current->owner;
    process->group_exiting = 1;
    for (unsigned i = 1; i < NPK_MAX_THREADS; ++i) {
        thread_t *thread = &threads[i];
        if (thread->owner != process || thread->state == THREAD_UNUSED) continue;
        clear_child_tid(thread);
        futex_cancel_thread(thread);
        thread->thread_exit_status = status;
        thread->state = THREAD_ZOMBIE;
    }
    process->thread_count = 0;
    process->exit_status = status;
    process->alive = 0;
    gop_release_user_display(process->pid);
    vm_revoke_device_mappings(process);
    destroy_process_memory(process);
    close_process_fds(process);
    discard_incoming_fd_transfers(process);
    wake_parent_waiter(process);
    LOG_INFOF("proc", "process exited", process->pid);
}

void process_exit_thread_current(uint8_t status) {
    if (!current || current == &threads[0] || !current->owner || !current->owner->alive) return;
    process_t *process = current->owner;
    if (process->group_exiting) return;
    clear_child_tid(current);
    futex_cancel_thread(current);
    current->thread_exit_status = status;
    current->state = THREAD_ZOMBIE;
    if (process->thread_count > 0) --process->thread_count;

    if (current == process->main_thread) {
        process->main_thread = NULL;
        for (unsigned i = 1; i < NPK_MAX_THREADS; ++i) {
            thread_t *candidate = &threads[i];
            if (candidate->owner != process || candidate->state == THREAD_ZOMBIE ||
                candidate->state == THREAD_UNUSED) continue;
            candidate->group_leader = 1;
            process->main_thread = candidate;
            break;
        }
    }
    if (process->thread_count == 0) {
        process->group_exiting = 1;
        process->exit_status = status;
        process->alive = 0;
        gop_release_user_display(process->pid);
        vm_revoke_device_mappings(process);
        destroy_process_memory(process);
        close_process_fds(process);
        discard_incoming_fd_transfers(process);
        wake_parent_waiter(process);
    }
}

int64_t process_set_tid_address(vaddr_t clear_child_tid) {
    if (!current || !current->owner || current->privilege_ring != 3) return -PROCESS_EPERM;
    if (clear_child_tid && !vmm_is_user_range(clear_child_tid, sizeof(uint32_t))) return -PROCESS_EFAULT;
    current->clear_child_tid = clear_child_tid;
    return (int64_t)current->tid;
}

int64_t process_wait4(int64_t target_pid, vaddr_t user_status, uint64_t options) {
    process_t *parent = process_current();
    if (!parent || !parent->alive || !current || current->privilege_ring != 3) return -PROCESS_EFAULT;
    if (target_pid == 0 || target_pid < -1 || (options & ~PROCESS_WNOHANG) != 0) return -PROCESS_EINVAL;
    if (user_status && !vmm_is_user_range(user_status, sizeof(uint32_t))) return -PROCESS_EFAULT;

    bool has_child = false;
    for (unsigned i = 1; i < NPK_MAX_PROCESSES; ++i) {
        process_t *child = &processes[i];
        if (child->pid == 0 || child->parent_pid != parent->pid) continue;
        if (!wait_target_matches(target_pid, child)) continue;
        has_child = true;
        if (child->alive) continue;
        uint32_t status = (uint32_t)child->exit_status << 8;
        if (user_status && !copyout_process(parent, user_status, &status, sizeof(status)))
            return -PROCESS_EFAULT;
        int64_t result = (int64_t)child->pid;
        child->waited = 1;
        reap_process(child);
        return result;
    }
    if (!has_child) return -PROCESS_ECHILD;
    if (options & PROCESS_WNOHANG) return 0;

    parent->wait_active = 1;
    parent->wait_woken = 0;
    parent->wait_result = -PROCESS_EAGAIN;
    parent->wait_target = target_pid;
    parent->wait_status_user = user_status;
    parent->wait_options = options;
    current->state = THREAD_BLOCKED;
    scheduler_yield();
    /* A child exit records the result before making this thread runnable. The
     * resumed waiter consumes that result and reaps the claimed zombie while
     * the parent still owns the process-table transition. */
    if (parent->wait_woken) {
        int64_t result = parent->wait_result;
        uint64_t child_pid = result > 0 ? (uint64_t)result : 0;
        parent->wait_active = 0;
        parent->wait_woken = 0;
        current->state = THREAD_RUNNING;
        if (child_pid != 0) {
            process_t *child = find_process_by_pid(child_pid);
            if (child && !child->alive && child->waited) reap_process(child);
        }
        return result;
    }
    /* scheduler_yield() may return when no runnable thread exists. */
    parent->wait_active = 0;
    current->state = THREAD_RUNNING;
    return -PROCESS_EAGAIN;
}

thread_t *scheduler_current(void) { return current; }
process_t *process_current(void) { return current ? current->owner : NULL; }

void process_set_image_path(process_t *process, const char *path) {
    if (!process) return;
    memset(process->image_path, 0, sizeof(process->image_path));
    if (!path) return;
    size_t length = strlen(path);
    if (length >= sizeof(process->image_path)) length = sizeof(process->image_path) - 1;
    memcpy(process->image_path, path, length);
    process->image_path[length] = '\0';
}

int64_t process_send_fd(int64_t target_pid, int source_fd) {
    process_t *sender = process_current();
    if (!sender || !sender->alive || sender == kernel_process || source_fd < 0 || source_fd >= NPK_MAX_FDS)
        return -PROCESS_EPERM;
    int handle = sender->fds[source_fd];
    if (handle < 0) return -PROCESS_EBADF;
    process_t *target = target_pid > 0 ? find_process_by_pid((uint64_t)target_pid) : NULL;
    if (!target || !target->alive) return -PROCESS_ESRCH;
    if (target != sender && target->parent_pid != sender->pid && sender->parent_pid != target->pid)
        return -PROCESS_EPERM;
    if (target->incoming_fd_count >= NPK_FD_TRANSFER_MAX) return -PROCESS_EAGAIN;
    if (vfs_retain(handle) != 0) return -PROCESS_EBADF;
    target->incoming_fd_transfers[target->incoming_fd_count].handle = handle;
    target->incoming_fd_transfers[target->incoming_fd_count].fd_flags = 0;
    ++target->incoming_fd_count;
    return 0;
}

int process_receive_fd(process_t *process, int *handle, uint32_t *fd_flags) {
    if (!process || !handle || !fd_flags || process->incoming_fd_count == 0) return -PROCESS_EAGAIN;
    *handle = process->incoming_fd_transfers[0].handle;
    *fd_flags = process->incoming_fd_transfers[0].fd_flags;
    for (uint32_t i = 1; i < process->incoming_fd_count; ++i)
        process->incoming_fd_transfers[i - 1] = process->incoming_fd_transfers[i];
    --process->incoming_fd_count;
    process->incoming_fd_transfers[process->incoming_fd_count].handle = -1;
    process->incoming_fd_transfers[process->incoming_fd_count].fd_flags = 0;
    return 0;
}

void scheduler_tick(void) {
    ++scheduler_ticks;
    futex_expire_waiters();
    if (!current) return;
    current->state = THREAD_RUNNING;
    reschedule_pending = next_runnable(current) != NULL;
}

uint64_t *scheduler_preempt(uint64_t *interrupt_frame) {
    if (!current || !interrupt_frame) return interrupt_frame;
    ++scheduler_ticks;
    futex_expire_waiters();
    /* The thread's nominal ring is not sufficient while the kernel is
     * preparing an IRET frame for a user thread: a PIT IRQ can arrive before
     * IRETQ and then carries a ring-0 CPU frame. The CS actually pushed by
     * the CPU is the authoritative privilege indicator. */
    bool interrupted_user = (interrupt_frame[16] & 3ULL) == 3ULL;
    if (!interrupted_user) {
        reschedule_pending = next_runnable(current) != NULL;
        return interrupt_frame;
    }
    current->saved_interrupt_frame = interrupt_frame;
    current->saved_frame_kind = 0;
    current->state = THREAD_RUNNING;
    thread_t *next_thread = next_runnable(current);
    if (!next_thread) {
        reschedule_pending = false;
        return interrupt_frame;
    }
    current->state = THREAD_READY;
    activate_thread(next_thread);
    reschedule_pending = false;
    if (!next_thread->saved_interrupt_frame)
        next_thread->saved_interrupt_frame = materialize_context_frame(next_thread);
    if (next_thread->saved_interrupt_frame && next_thread->saved_frame_kind == 0 &&
        (next_thread->saved_interrupt_frame[16] & 3ULL) == 3ULL)
        (void)process_deliver_pending_signal(next_thread->saved_interrupt_frame);
    return next_thread->saved_interrupt_frame ? next_thread->saved_interrupt_frame : interrupt_frame;
}

bool scheduler_reschedule_pending(void) { return reschedule_pending; }

void scheduler_yield(void) {
    /* current is changed before the final RET/IRET transfer. Keep the
     * scheduler critical section atomic or a timer IRQ can save its kernel
     * call-site as if it were the user thread's return frame. */
    __asm__ volatile("cli" ::: "memory");
    thread_t *old_thread = current;
    process_t *old_process = old_thread ? old_thread->owner : NULL;
    bool old_user_thread = old_thread && old_thread->privilege_ring == 3;
    thread_t *next_thread = next_runnable(old_thread);
    if (!old_thread || !next_thread || next_thread == old_thread) {
        reschedule_pending = false;
        /* A terminating ring-3 syscall falls through to arch_halt(). Do not
         * re-enable IRQ0 while its SYSCALL frame is still active; a timer
         * interrupt would try to IRET through a kernel-only call stack. */
        if (!old_thread || old_thread->privilege_ring == 0)
            __asm__ volatile("sti" ::: "memory");
        return;
    }
    bool blocked_old_thread = old_thread->state == THREAD_BLOCKED;
    if (old_thread->state == THREAD_RUNNING) old_thread->state = THREAD_READY;
    if (!blocked_old_thread) old_thread->saved_interrupt_frame = NULL;
    activate_thread(next_thread);
    if (old_process && old_process->waited && !old_process->alive)
        reap_process(old_process);
    reschedule_pending = false;
    if (next_thread->privilege_ring == 0) {
        /* A terminating syscall cannot return through SYSRET. The target
         * kernel thread already has a SysV-aligned RET bootstrap stack. */
        context_switch(&old_thread->context, &next_thread->context);
        return;
    }
    if (!next_thread->saved_interrupt_frame)
        next_thread->saved_interrupt_frame = materialize_context_frame(next_thread);
    if (next_thread->saved_interrupt_frame) {
        if (next_thread->saved_frame_kind == 0 &&
            (next_thread->saved_interrupt_frame[16] & 3ULL) == 3ULL)
            (void)process_deliver_pending_signal(next_thread->saved_interrupt_frame);
        if (old_user_thread)
            restore_user_frame_from_kernel(next_thread->saved_interrupt_frame);
        restore_interrupt_frame(next_thread->saved_interrupt_frame);
    }
    context_switch(&old_thread->context, &next_thread->context);
}

thread_t *scheduler_create_kernel_thread(void (*entry)(void *), void *argument) {
    if (!kernel_process || !entry) return NULL;
    thread_t *thread = allocate_thread();
    if (!thread) return NULL;
    thread->kernel_stack = (uint64_t)kcalloc(1, NPK_KERNEL_STACK_SIZE);
    if (!thread->kernel_stack) {
        memset(thread, 0, sizeof(*thread));
        return NULL;
    }
    uint64_t stack_top = (thread->kernel_stack + NPK_KERNEL_STACK_SIZE) & ~0xFULL;
    thread->kernel_stack_top = stack_top;
    thread->privilege_ring = 0;
    thread->state = THREAD_READY;
    thread->owner = kernel_process;
    thread->entry = entry;
    thread->argument = argument;
    thread->context.rip = (uint64_t)scheduler_thread_bootstrap;
    thread->context.rsp = stack_top - 2 * sizeof(uint64_t);
    *(uint64_t *)(stack_top - 2 * sizeof(uint64_t)) = (uint64_t)scheduler_thread_bootstrap;
    thread->context.rflags = 0x202;
    thread->context.cr3 = vmm_current_root();
    return thread;
}

void process_launch_user(process_t *process) {
    if (!process || !process->alive || !process->main_thread || process->main_thread->privilege_ring != 3) return;
    process_activate(process);
    /* The kernel invariant has GS_BASE=CPU and KERNEL_GS_BASE=user GS;
     * swap before the initial IRET so the first user SYSCALL can swap back. */
    restore_user_frame_from_kernel(process->main_thread->saved_interrupt_frame);
    panic("returned from user mode");
}
