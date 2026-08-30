#include <npk/boot.h>
#include <npk/font.h>
#include <npk/gop.h>
#include <npk/string.h>

static struct limine_framebuffer *fb;
static uint32_t current_bg;
static uint64_t user_display_owner;

static uint32_t channel(uint32_t value, uint8_t shift, uint8_t size) {
    if (size == 0) return 0;
    if (size < 8) value >>= 8 - size;
    return (value & ((1U << size) - 1U)) << shift;
}

static uint32_t pixel_value(uint32_t rgb) {
    if (fb->memory_model == LIMINE_FRAMEBUFFER_RGB) {
        uint32_t red = (rgb >> 16) & 0xffU;
        uint32_t green = (rgb >> 8) & 0xffU;
        uint32_t blue = rgb & 0xffU;
        return channel(red, fb->red_mask_shift, fb->red_mask_size) |
               channel(green, fb->green_mask_shift, fb->green_mask_size) |
               channel(blue, fb->blue_mask_shift, fb->blue_mask_size);
    }
    return rgb;
}

static void put_pixel(uint64_t x, uint64_t y, uint32_t rgb) {
    if (fb == NULL || x >= fb->width || y >= fb->height) return;
    uint8_t *row = (uint8_t *)fb->address + y * fb->pitch;
    uint32_t value = pixel_value(rgb);
    if (fb->bpp == 32) {
        ((uint32_t *)row)[x] = value;
    } else if (fb->bpp == 24) {
        uint8_t *pixel = row + x * 3;
        pixel[0] = (uint8_t)value;
        pixel[1] = (uint8_t)(value >> 8);
        pixel[2] = (uint8_t)(value >> 16);
    } else if (fb->bpp == 16) {
        ((uint16_t *)row)[x] = (uint16_t)value;
    }
}

static void fill_rows(uint64_t first_row, uint64_t last_row, uint32_t color) {
    if (fb == NULL || first_row >= last_row || first_row >= fb->height) return;
    if (last_row > fb->height) last_row = fb->height;
    for (uint64_t y = first_row; y < last_row; ++y)
        for (uint64_t x = 0; x < fb->width; ++x)
            put_pixel(x, y, color);
}

bool gop_init(void) {
    fb = g_boot_info.framebuffer;
    if (fb == NULL || fb->address == NULL || fb->width == 0 || fb->height == 0) return false;
    if (fb->bpp != 16 && fb->bpp != 24 && fb->bpp != 32) return false;
    current_bg = 0x101820;
    user_display_owner = 0;
    return true;
}

bool gop_ready(void) { return fb != NULL; }

bool gop_get_framebuffer_info(gop_framebuffer_info_t *info) {
    if (!fb || !info || fb->address == NULL || fb->width == 0 || fb->height == 0 || fb->pitch == 0) return false;
    uint64_t raw_address = (uint64_t)fb->address;
    uint64_t physical = raw_address;
    if (g_boot_info.hhdm && raw_address >= g_boot_info.hhdm->offset)
        physical = raw_address - g_boot_info.hhdm->offset;
    uint64_t page_offset = physical & (NPK_PAGE_SIZE - 1);
    uint64_t page_base = physical - page_offset;
    if (fb->height > UINT64_MAX / fb->pitch) return false;
    uint64_t byte_count = fb->pitch * fb->height;
    if (byte_count > UINT64_MAX - page_offset) return false;
    uint64_t mapping_size = page_offset + byte_count;
    if (mapping_size > UINT64_MAX - (NPK_PAGE_SIZE - 1)) return false;
    mapping_size = (mapping_size + NPK_PAGE_SIZE - 1) & ~(NPK_PAGE_SIZE - 1);
    if (mapping_size == 0) return false;
    *info = (gop_framebuffer_info_t){
        .physical_base = page_base,
        .page_offset = page_offset,
        .mapping_size = mapping_size,
        .width = fb->width,
        .height = fb->height,
        .pitch = fb->pitch,
        .bpp = fb->bpp,
        .memory_model = fb->memory_model,
        .red_mask_size = fb->red_mask_size,
        .red_mask_shift = fb->red_mask_shift,
        .green_mask_size = fb->green_mask_size,
        .green_mask_shift = fb->green_mask_shift,
        .blue_mask_size = fb->blue_mask_size,
        .blue_mask_shift = fb->blue_mask_shift,
    };
    return true;
}

bool gop_claim_user_display(uint64_t pid) {
    if (!gop_ready() || pid == 0) return false;
    if (user_display_owner != 0 && user_display_owner != pid) return false;
    user_display_owner = pid;
    return true;
}

void gop_release_user_display(uint64_t pid) {
    if (pid != 0 && user_display_owner == pid) user_display_owner = 0;
}

bool gop_user_display_owned(void) { return user_display_owner != 0; }
bool gop_user_display_owned_by(uint64_t pid) { return pid != 0 && user_display_owner == pid; }

uint64_t gop_width(void) { return fb != NULL ? fb->width : 0; }
uint64_t gop_height(void) { return fb != NULL ? fb->height : 0; }
uint64_t gop_cell_width(void) { return NPK_FONT_WIDTH * NPK_TTY_SCALE; }
uint64_t gop_cell_height(void) { return NPK_FONT_HEIGHT * NPK_TTY_SCALE; }
uint64_t gop_columns(void) { return fb != NULL ? fb->width / gop_cell_width() : 0; }
uint64_t gop_rows(void) { return fb != NULL ? fb->height / gop_cell_height() : 0; }

void gop_clear(uint32_t color) {
    if (fb == NULL || user_display_owner != 0) return;
    current_bg = color;
    fill_rows(0, fb->height, color);
}

void gop_draw_cell(uint64_t column, uint64_t row, uint32_t codepoint, uint32_t fg, uint32_t bg) {
    if (fb == NULL || user_display_owner != 0 || column >= gop_columns() || row >= gop_rows()) return;
    const uint16_t *glyph = font_glyph(codepoint);
    const uint64_t origin_x = column * gop_cell_width();
    const uint64_t origin_y = row * gop_cell_height();
    for (uint64_t gy = 0; gy < NPK_FONT_HEIGHT; ++gy) {
        uint16_t bits = glyph[gy];
        for (uint64_t gx = 0; gx < NPK_FONT_WIDTH; ++gx) {
            uint32_t color = (bits & (1U << gx)) != 0 ? fg : bg;
            for (uint64_t sy = 0; sy < NPK_TTY_SCALE; ++sy)
                for (uint64_t sx = 0; sx < NPK_TTY_SCALE; ++sx)
                    put_pixel(origin_x + gx * NPK_TTY_SCALE + sx,
                              origin_y + gy * NPK_TTY_SCALE + sy,
                              color);
        }
    }
}

void gop_scroll_rows(uint64_t rows, uint32_t bg) {
    if (fb == NULL || user_display_owner != 0 || rows == 0) return;
    uint64_t pixel_rows = rows * gop_cell_height();
    if (pixel_rows >= fb->height) {
        gop_clear(bg);
        return;
    }
    size_t byte_count = (size_t)(fb->height - pixel_rows) * (size_t)fb->pitch;
    memmove(fb->address,
            (uint8_t *)fb->address + (size_t)pixel_rows * (size_t)fb->pitch,
            byte_count);
    fill_rows(fb->height - pixel_rows, fb->height, bg);
    current_bg = bg;
}
