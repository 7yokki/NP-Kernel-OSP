#include <npk/arch.h>
#include <npk/types.h>
#include <npk/memory.h>

#define VGA_MEMORY ((volatile uint16_t *)phys_to_virt(0xb8000))
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static uint8_t row;
static uint8_t column;
static uint8_t color = 0x07;

void vga_init(void) {
    row = 0; column = 0;
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i) VGA_MEMORY[i] = ((uint16_t)color << 8) | ' ';
}

void vga_set_color(uint8_t fg, uint8_t bg) { color = (bg << 4) | (fg & 0x0f); }
void vga_clear(void) {
    for (size_t i = 0; i < VGA_WIDTH * VGA_HEIGHT; ++i) VGA_MEMORY[i] = ((uint16_t)color << 8) | ' ';
    row = column = 0;
}

void vga_write(const char *text) {
    while (*text) {
        char c = *text++;
        if (c == '\n') { column = 0; if (++row == VGA_HEIGHT) row = 0; continue; }
        VGA_MEMORY[row * VGA_WIDTH + column] = ((uint16_t)color << 8) | (uint8_t)c;
        if (++column == VGA_WIDTH) { column = 0; if (++row == VGA_HEIGHT) row = 0; }
    }
}
