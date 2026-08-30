#ifndef NPK_CONSOLE_H
#define NPK_CONSOLE_H

#include "types.h"

void console_init(void);
void console_clear(void);
void console_putc(char c);
void console_write(const char *s);
void console_write_n(const char *s, size_t n);
void console_write_u64(uint64_t value, unsigned base);
void console_write_i64(int64_t value);
void console_write_hex(uint64_t value);
void console_set_color(uint32_t fg, uint32_t bg);

#endif
