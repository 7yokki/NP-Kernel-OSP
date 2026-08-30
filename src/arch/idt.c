#include <npk/arch.h>
#include <npk/log.h>
#include <npk/panic.h>
#include <npk/process.h>
#include <npk/vm.h>

struct idt_gate {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;
    uint8_t type_attributes;
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t reserved;
} __attribute__((packed));
struct idtr { uint16_t limit; uint64_t base; } __attribute__((packed));

extern void isr_stub_default(void);
extern void isr_stub_divide_error(void);
extern void isr_stub_debug(void);
extern void isr_stub_nmi(void);
extern void isr_stub_breakpoint(void);
extern void isr_stub_overflow(void);
extern void isr_stub_bound_range(void);
extern void isr_stub_invalid_opcode(void);
extern void isr_stub_device_not_available(void);
extern void isr_stub_double_fault(void);
extern void isr_stub_coprocessor_overrun(void);
extern void isr_stub_invalid_tss(void);
extern void isr_stub_segment_not_present(void);
extern void isr_stub_stack_segment(void);
extern void isr_stub_general_protection(void);
extern void isr_stub_page_fault(void);
extern void isr_stub_x87(void);
extern void isr_stub_alignment_check(void);
extern void isr_stub_machine_check(void);
extern void isr_stub_simd(void);
extern void isr_stub_virtualization(void);
extern void isr_stub_control_protection(void);
extern void irq1_stub(void);
extern void irq12_stub(void);
extern void irq9_stub(void);
extern void irq_timer_stub(void);
static struct idt_gate idt[256];

static void set_gate(unsigned vector, void (*handler)(void), uint8_t attributes) {
    uint64_t address = (uint64_t)handler;
    idt[vector].offset_low = address & 0xffff;
    idt[vector].selector = 0x08;
    idt[vector].ist = 0;
    idt[vector].type_attributes = attributes;
    idt[vector].offset_mid = (address >> 16) & 0xffff;
    idt[vector].offset_high = address >> 32;
    idt[vector].reserved = 0;
}

static __attribute__((noreturn)) void terminate_user_exception(uint64_t signal) {
    uint64_t status = 128ULL + signal;
    process_exit_current((uint8_t)(status > 255 ? 255 : status));
    LOG_ERRORF("exception", "user process terminated by signal", signal);
    scheduler_yield();
    arch_halt();
    __builtin_unreachable();
}

static uint64_t exception_signal(uint64_t vector) {
    switch (vector) {
        case 1:  return 5;  /* SIGTRAP: debug */
        case 4:  return 8;  /* SIGFPE: overflow */
        case 5:  return 8;  /* SIGFPE: bound range */
        case 6:  return 4;  /* SIGILL: invalid opcode */
        case 7:  return 8;  /* SIGFPE: device unavailable */
        case 8:  return 11; /* SIGSEGV: double fault */
        case 11: return 11; /* SIGSEGV: segment-not-present */
        case 12: return 11; /* SIGSEGV: stack-segment */
        case 13: return 11; /* SIGSEGV: general protection */
        case 14: return 11; /* SIGSEGV: page fault */
        case 16: return 8;  /* SIGFPE: x87 */
        case 17: return 7;  /* SIGBUS: alignment */
        case 18: return 7;  /* SIGBUS: machine check */
        case 19: return 8;  /* SIGFPE: SIMD */
        case 20: return 4;  /* SIGILL: virtualization */
        case 21: return 11; /* SIGSEGV: control protection */
        default: return 7;  /* conservative hardware-fault default */
    }
}

static uint64_t frame_word(const uint64_t *wrapper, uint64_t has_error, uint64_t index) {
    return wrapper ? wrapper[15 + has_error + index] : 0;
}

void page_fault_dispatch(uint64_t *wrapper) {
    uint64_t error = wrapper ? wrapper[15] : 0;
    uint64_t rip = frame_word(wrapper, 1, 0);
    uint64_t cs = frame_word(wrapper, 1, 1);
    uint64_t fault_address = read_cr2();
    LOG_ERRORF("page_fault", "fault address", fault_address);
    LOG_ERRORF("page_fault", "instruction pointer", rip);
    LOG_ERRORF("page_fault", "error code", error);
    LOG_ERRORF("page_fault", "code segment", cs);
    if ((cs & 3) == 3) {
        process_t *process = process_current();
        vaddr_t user_rsp = frame_word(wrapper, 1, 3);
        if (process && vm_handle_page_fault(process, fault_address, error, user_rsp)) return;
        if (process_deliver_signal(wrapper, 11)) return;
        terminate_user_exception(11);
    }
    panic("fatal page fault");
}

void idt_init(void) {
    for (unsigned i = 0; i < 256; ++i) set_gate(i, isr_stub_default, 0x8e);
    set_gate(0, isr_stub_divide_error, 0x8e);
    set_gate(1, isr_stub_debug, 0x8e);
    set_gate(2, isr_stub_nmi, 0x8e);
    set_gate(3, isr_stub_breakpoint, 0x8e);
    set_gate(4, isr_stub_overflow, 0x8e);
    set_gate(5, isr_stub_bound_range, 0x8e);
    set_gate(6, isr_stub_invalid_opcode, 0x8e);
    set_gate(7, isr_stub_device_not_available, 0x8e);
    set_gate(8, isr_stub_double_fault, 0x8e);
    set_gate(9, isr_stub_coprocessor_overrun, 0x8e);
    set_gate(10, isr_stub_invalid_tss, 0x8e);
    set_gate(11, isr_stub_segment_not_present, 0x8e);
    set_gate(12, isr_stub_stack_segment, 0x8e);
    set_gate(13, isr_stub_general_protection, 0x8e);
    set_gate(14, isr_stub_page_fault, 0x8e);
    /* Vector 15 is reserved; leave the safe default gate installed. */
    set_gate(16, isr_stub_x87, 0x8e);
    set_gate(17, isr_stub_alignment_check, 0x8e);
    set_gate(18, isr_stub_machine_check, 0x8e);
    set_gate(19, isr_stub_simd, 0x8e);
    set_gate(20, isr_stub_virtualization, 0x8e);
    set_gate(21, isr_stub_control_protection, 0x8e);
    set_gate(32, irq_timer_stub, 0x8e);
    set_gate(33, irq1_stub, 0x8e);
    set_gate(44, irq12_stub, 0x8e); /* legacy PIC IRQ12, PS/2 mouse */
    set_gate(41, irq9_stub, 0x8e); /* legacy PIC IRQ9, normal ACPI SCI route */
    set_gate(14, isr_stub_page_fault, 0x8e);
    idt[2].ist = 1;  /* NMI */
    idt[8].ist = 1;  /* double fault */
    idt[13].ist = 1; /* general protection */
    idt[14].ist = 1; /* page fault */
    idt_load_current();
    LOG_INFOF("idt", "gates installed", 256);
}

void idt_load_current(void) {
    struct idtr descriptor = { .limit = sizeof(idt) - 1, .base = (uint64_t)idt };
    __asm__ volatile("lidt %0" : : "m"(descriptor) : "memory");
}

void general_protection_dispatch(uint64_t *wrapper) {
    uint64_t error = wrapper ? wrapper[15] : 0;
    uint64_t rip = frame_word(wrapper, 1, 0);
    uint64_t cs = frame_word(wrapper, 1, 1);
    uint64_t rflags = frame_word(wrapper, 1, 2);
    uint64_t rsp = frame_word(wrapper, 1, 3);
    uint64_t ss = frame_word(wrapper, 1, 4);
    LOG_ERRORF("#gp", "error code", error);
    LOG_ERRORF("#gp", "fault RIP", rip);
    LOG_ERRORF("#gp", "fault CS", cs);
    LOG_ERRORF("#gp", "fault RFLAGS", rflags);
    LOG_ERRORF("#gp", "fault RSP", rsp);
    LOG_ERRORF("#gp", "fault SS", ss);
    if ((cs & 3) == 3) {
        if (process_deliver_signal(wrapper, 11)) return;
        terminate_user_exception(11);
    }
    panic_with_code("general protection fault", error);
}

void exception_dispatch(uint64_t *wrapper, uint64_t vector, uint64_t has_error) {
    uint64_t error = has_error && wrapper ? wrapper[15] : 0;
    uint64_t rip = frame_word(wrapper, has_error, 0);
    uint64_t cs = frame_word(wrapper, has_error, 1);
    uint64_t rflags = frame_word(wrapper, has_error, 2);
    uint64_t rsp = frame_word(wrapper, has_error, 3);
    uint64_t ss = frame_word(wrapper, has_error, 4);
    LOG_ERRORF("exception", "vector", vector);
    LOG_ERRORF("exception", "instruction pointer", rip);
    LOG_ERRORF("exception", "code segment", cs);
    LOG_ERRORF("exception", "error code", error);
    if ((cs & 3) == 3) {
        uint64_t signal = exception_signal(vector);
        if (process_deliver_signal(wrapper, signal)) return;
        terminate_user_exception(signal);
    }
    (void)rflags;
    (void)rsp;
    (void)ss;
    panic_with_code("CPU exception", vector);
}

void arch_halt(void) {
    for (;;) __asm__ volatile("hlt");
}
