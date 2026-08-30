#include <npk/panic.h>
#include <npk/console.h>
#include <npk/log.h>
#include <npk/arch.h>

static NPK_NORETURN void halt_forever(void) {
    __asm__ volatile("cli");
    for (;;) __asm__ volatile("hlt");
}

NPK_NORETURN void panic(const char *reason) {
    console_set_color(0xffffff, 0x7f0000);
    console_write("\n\n!!! NPKERNEL PANIC !!!\n");
    console_write(reason);
    console_putc('\n');
    log_message(LOG_FATAL, "panic", reason);
    halt_forever();
}

NPK_NORETURN void panic_with_code(const char *reason, uint64_t code) {
    console_set_color(0xffffff, 0x7f0000);
    console_write("\n\n!!! NPKERNEL PANIC !!!\n");
    console_write(reason);
    console_write(" code=");
    console_write_hex(code);
    console_putc('\n');
    logf(LOG_FATAL, "panic", reason, code);
    halt_forever();
}
