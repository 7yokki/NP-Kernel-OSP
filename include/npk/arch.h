#ifndef NPK_ARCH_H
#define NPK_ARCH_H

#include "types.h"

void arch_init(void);
void arch_init_cpu(uint32_t cpu_index);
void gdt_init(void);
void gdt_init_cpu(uint32_t cpu_index);
void tss_set_rsp0(uint64_t stack_top);
uint32_t arch_cpu_index(void);
void idt_init(void);
void idt_load_current(void);
void syscall_cpu_init(void);
void exception_dispatch(uint64_t *wrapper_frame, uint64_t vector, uint64_t has_error);
void general_protection_dispatch(uint64_t *wrapper_frame);
void page_fault_dispatch(uint64_t *wrapper_frame);
void irq_init(void);
void irq1_dispatch(void);
void irq12_dispatch(void);
void arch_halt(void);

#define NPK_IA32_FS_BASE 0xc0000100U
#define NPK_IA32_GS_BASE 0xc0000101U
#define NPK_IA32_KERNEL_GS_BASE 0xc0000102U

void arch_set_fs_base(uint64_t base);
uint64_t arch_get_fs_base(void);
void arch_set_user_gs_base(uint64_t base);
uint64_t arch_get_user_gs_base(void);

static inline void outb(uint16_t port, uint8_t value) {
    __asm__ volatile ("outb %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint8_t inb(uint16_t port) {
    uint8_t value;
    __asm__ volatile ("in %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void outl(uint16_t port, uint32_t value) {
    __asm__ volatile ("outl %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint32_t inl(uint16_t port) {
    uint32_t value;
    __asm__ volatile ("inl %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline void outw(uint16_t port, uint16_t value) {
    __asm__ volatile ("outw %0, %1" : : "a"(value), "Nd"(port));
}
static inline uint16_t inw(uint16_t port) {
    uint16_t value;
    __asm__ volatile ("inw %1, %0" : "=a"(value) : "Nd"(port));
    return value;
}
static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t lo, hi;
    __asm__ volatile ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}
static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t lo = (uint32_t)value, hi = (uint32_t)(value >> 32);
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(lo), "d"(hi));
}
static inline uint64_t read_cr2(void) {
    uint64_t v;
    __asm__ volatile ("mov %%cr2, %0" : "=r"(v));
    return v;
}

#endif
