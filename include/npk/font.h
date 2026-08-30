#ifndef NPK_FONT_H
#define NPK_FONT_H

#include <stdint.h>

#define NPK_FONT_WIDTH 10U
#define NPK_FONT_HEIGHT 16U

const uint16_t *font_glyph(uint32_t codepoint);

#endif
