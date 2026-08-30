#include <npk/gop.h>
#include <npk/boot.h>
#include <npk/arch.h>
#include <npk/memory.h>

static uint64_t cursor_x;
static uint64_t cursor_y;
static uint32_t foreground = 0xf2f5fa;
static uint32_t background = 0x101820;
static bool using_gop;
static volatile uint16_t *vga_buffer;

static uint16_t vga_cursor_x;
static uint16_t vga_cursor_y;

static void vga_putc_fallback(char c);

static void tty_newline(void) {
    cursor_x = 0;
    ++cursor_y;
    if (cursor_y >= gop_rows()) {
        gop_scroll_rows(1, background);
        cursor_y = gop_rows() == 0 ? 0 : gop_rows() - 1;
    }
}

static void draw_codepoint(uint32_t cp) {
    if (!using_gop) {
        if (cp < 128) vga_putc_fallback((char)cp);
        return;
    }

    const uint64_t columns = gop_columns();
    if (columns == 0 || gop_rows() == 0) return;

    if (cp == '\n') {
        tty_newline();
    } else if (cp == '\r') {
        cursor_x = 0;
    } else if (cp == '\t') {
        uint64_t spaces = 8 - (cursor_x & 7ULL);
        for (uint64_t i = 0; i < spaces; ++i) {
            draw_codepoint(' ');
            if (cursor_x == 0) break;
        }
    } else if (cp == '\b') {
        if (cursor_x != 0) {
            --cursor_x;
            gop_draw_cell(cursor_x, cursor_y, ' ', foreground, background);
        }
    } else {
        gop_draw_cell(cursor_x, cursor_y, cp, foreground, background);
        ++cursor_x;
        if (cursor_x >= columns) tty_newline();
    }
}

void console_init(void) {
    vga_buffer = (volatile uint16_t *)phys_to_virt(0xb8000);
    using_gop = gop_init();
    cursor_x = cursor_y = 0;
    vga_cursor_x = vga_cursor_y = 0;
    if (using_gop) gop_clear(background);
}

void console_clear(void) {
    cursor_x = cursor_y = 0;
    vga_cursor_x = vga_cursor_y = 0;
    if (using_gop) gop_clear(background);
}

void console_set_color(uint32_t fg, uint32_t bg) {
    foreground = fg;
    background = bg;
}

void console_putc(char c) { draw_codepoint((uint8_t)c); }

void console_write_n(const char *s, size_t n) {
    for (size_t i = 0; i < n; ++i) {
        uint8_t c = (uint8_t)s[i];
        uint32_t cp = c;
        size_t consumed = 1;
        if ((c & 0xe0) == 0xc0 && i + 1 < n && ((uint8_t)s[i + 1] & 0xc0) == 0x80) {
            cp = ((uint32_t)(c & 0x1f) << 6) | ((uint8_t)s[i + 1] & 0x3f);
            consumed = 2;
        } else if ((c & 0xf0) == 0xe0 && i + 2 < n &&
                   ((uint8_t)s[i + 1] & 0xc0) == 0x80 && ((uint8_t)s[i + 2] & 0xc0) == 0x80) {
            cp = ((uint32_t)(c & 0x0f) << 12) |
                 ((uint32_t)((uint8_t)s[i + 1] & 0x3f) << 6) |
                 ((uint8_t)s[i + 2] & 0x3f);
            consumed = 3;
        } else if ((c & 0xf8) == 0xf0 && i + 3 < n &&
                   ((uint8_t)s[i + 1] & 0xc0) == 0x80 &&
                   ((uint8_t)s[i + 2] & 0xc0) == 0x80 &&
                   ((uint8_t)s[i + 3] & 0xc0) == 0x80) {
            cp = ((uint32_t)(c & 7) << 18) |
                 ((uint32_t)((uint8_t)s[i + 1] & 0x3f) << 12) |
                 ((uint32_t)((uint8_t)s[i + 2] & 0x3f) << 6) |
                 ((uint8_t)s[i + 3] & 0x3f);
            consumed = 4;
        }
        draw_codepoint(cp);
        i += consumed - 1;
    }
}

void console_write(const char *s) {
    size_t n = 0;
    while (s[n]) ++n;
    console_write_n(s, n);
}

void console_write_u64(uint64_t value, unsigned base) {
    char buffer[32];
    const char *digits = "0123456789abcdef";
    size_t i = 0;
    if (value == 0) { console_putc('0'); return; }
    while (value) { buffer[i++] = digits[value % base]; value /= base; }
    while (i) console_putc(buffer[--i]);
}

void console_write_hex(uint64_t value) {
    console_write("0x");
    console_write_u64(value, 16);
}

void console_write_i64(int64_t value) {
    if (value < 0) { console_putc('-'); console_write_u64((uint64_t)(-(value + 1)) + 1, 10); }
    else console_write_u64((uint64_t)value, 10);
}

static void vga_putc_fallback(char c) {
    if (c == '\n') { vga_cursor_x = 0; ++vga_cursor_y; }
    else if (c == '\r') vga_cursor_x = 0;
    else if (c == '\b') {
        if (vga_cursor_x != 0) --vga_cursor_x;
        vga_buffer[vga_cursor_y * 80 + vga_cursor_x] = 0x0720;
    } else if (c == '\t') {
        vga_cursor_x = (vga_cursor_x + 8) & ~7U;
    } else {
        vga_buffer[vga_cursor_y * 80 + vga_cursor_x] = (uint16_t)(0x07 << 8) | (uint8_t)c;
        ++vga_cursor_x;
    }
    if (vga_cursor_x >= 80) { vga_cursor_x = 0; ++vga_cursor_y; }
    if (vga_cursor_y >= 25) vga_cursor_y = 0;
}
