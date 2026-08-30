#include <npk/arch.h>
#include <npk/acpi.h>

extern void restore_kernel_frame(uint64_t *frame);
#include <npk/keyboard.h>
#include <npk/log.h>
#include <npk/mouse.h>
#include <npk/process.h>
#include <npk/pci.h>
#include <npk/timer.h>

static void pic_remap(void) {
    uint8_t master_mask = inb(0x21), slave_mask = inb(0xa1);
    outb(0x20, 0x11); outb(0xa0, 0x11);
    outb(0x21, 0x20); outb(0xa1, 0x28);
    outb(0x21, 0x04); outb(0xa1, 0x02);
    outb(0x21, 0x01); outb(0xa1, 0x01);
    outb(0x21, master_mask & (uint8_t)~0x02);
    outb(0xa1, slave_mask);
}

void irq_unmask_line(uint8_t irq) {
    if (irq < 8) {
        outb(0x21, (uint8_t)(inb(0x21) & (uint8_t)~(1U << irq)));
    } else if (irq < 16) {
        outb(0xa1, (uint8_t)(inb(0xa1) & (uint8_t)~(1U << (irq - 8))));
        outb(0x21, (uint8_t)(inb(0x21) & (uint8_t)~(1U << 2)));
    }
}

void irq_init(void) {
    pic_remap();
    irq_unmask_line(0);
    irq_unmask_line(1);
    mouse_init();
    if (mouse_ready()) irq_unmask_line(12);
    LOG_INFOF("irq", "PIC timer vector", 32);
    LOG_INFOF("irq", "PIC keyboard vector", 33);
}

void irq1_dispatch(void) {
    keyboard_irq();
    outb(0x20, 0x20);
}

void irq12_dispatch(void) {
    mouse_irq();
    outb(0xa0, 0x20);
    outb(0x20, 0x20);
}

uint64_t *irq_timer_dispatch(uint64_t *frame) {
    timer_irq();
    uint64_t *selected = scheduler_preempt(frame);
    outb(0x20, 0x20);
    if (selected && selected != frame && scheduler_current() && scheduler_current()->privilege_ring == 0) {
        restore_kernel_frame(selected);
        for (;;) __asm__ volatile("hlt");
    }
    return selected ? selected : frame;
}

void irq9_dispatch(void) {
    acpi_irq_dispatch();
    outb(0xa0, 0x20);
    outb(0x20, 0x20);
}

void arch_init(void) {
    gdt_init();
    idt_init();
    irq_init();
    timer_init(100);
    syscall_cpu_init();
    acpi_init();
    pci_init();
    if (acpi_sci_irq() == 9) {
        irq_unmask_line(9);
        LOG_INFOF("irq", "ACPI SCI vector", 41);
    } else if (acpi_sci_irq() != 0xff) {
        LOG_INFOF("irq", "ACPI SCI not routed by legacy PIC", acpi_sci_irq());
    }
}
