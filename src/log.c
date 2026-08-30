#include <npk/log.h>
#include <npk/console.h>
#include <npk/arch.h>

static const char *level_name(log_level_t level) {
    switch (level) {
        case LOG_TRACE: return "TRACE";
        case LOG_INFO: return "INFO";
        case LOG_WARN: return "WARN";
        case LOG_ERROR: return "ERROR";
        default: return "FATAL";
    }
}

static void serial_init(void) {
    outb(0x3f9, 0x00);
    outb(0x3fb, 0x80);
    outb(0x3f8, 0x03);
    outb(0x3f9, 0x00);
    outb(0x3fb, 0x03);
    outb(0x3fa, 0xc7);
    outb(0x3fc, 0x0b);
}

static void serial_putc(char c) {
    for (uint32_t spin = 0; spin < 100000; ++spin)
        if (inb(0x3fd) & 0x20) { outb(0x3f8, (uint8_t)c); return; }
}

static void serial_write(const char *s) {
    while (*s) serial_putc(*s++);
}

void log_init(void) { serial_init(); }

void log_message(log_level_t level, const char *component, const char *message) {
    console_write("["); console_write(level_name(level)); console_write("] ");
    console_write(component); console_write(": "); console_write(message); console_putc('\n');
    serial_write("["); serial_write(level_name(level)); serial_write("] ");
    serial_write(component); serial_write(": "); serial_write(message); serial_putc('\n');
}

void logf(log_level_t level, const char *component, const char *message, uint64_t value) {
    log_message(level, component, message);
    console_write("       value="); console_write_hex(value); console_putc('\n');
    serial_write("       value="); serial_write("0x");
    for (int i = 15; i >= 0; --i) {
        uint8_t digit = (value >> (i * 4)) & 0xf;
        serial_putc("0123456789abcdef"[digit]);
    }
    serial_putc('\n');
}
