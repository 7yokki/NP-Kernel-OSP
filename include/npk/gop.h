#ifndef NPK_GOP_H
#define NPK_GOP_H

#include "types.h"

#define NPK_TTY_SCALE 2U

typedef struct {
    paddr_t physical_base;
    uint64_t page_offset;
    uint64_t mapping_size;
    uint64_t width;
    uint64_t height;
    uint64_t pitch;
    uint16_t bpp;
    uint8_t memory_model;
    uint8_t red_mask_size;
    uint8_t red_mask_shift;
    uint8_t green_mask_size;
    uint8_t green_mask_shift;
    uint8_t blue_mask_size;
    uint8_t blue_mask_shift;
} gop_framebuffer_info_t;

bool gop_init(void);
bool gop_ready(void);
bool gop_get_framebuffer_info(gop_framebuffer_info_t *info);
bool gop_claim_user_display(uint64_t pid);
void gop_release_user_display(uint64_t pid);
bool gop_user_display_owned(void);
bool gop_user_display_owned_by(uint64_t pid);
uint64_t gop_width(void);
uint64_t gop_height(void);
uint64_t gop_cell_width(void);
uint64_t gop_cell_height(void);
uint64_t gop_columns(void);
uint64_t gop_rows(void);
void gop_clear(uint32_t color);
void gop_draw_cell(uint64_t column, uint64_t row, uint32_t codepoint, uint32_t fg, uint32_t bg);
void gop_scroll_rows(uint64_t rows, uint32_t bg);

#endif
