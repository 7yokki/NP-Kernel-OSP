from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

FONT_PATH = '/usr/share/fonts/truetype/dejavu/DejaVuSansMono-Bold.ttf'
FONT_SIZE = 15
FONT_WIDTH = 10
FONT_HEIGHT = 16
THRESHOLD = 112

CODEPOINTS = list(range(128)) + [
    0x00c7, 0x00e7, 0x011e, 0x011f, 0x0130, 0x0131,
    0x00d6, 0x00f6, 0x015e, 0x015f, 0x00dc, 0x00fc,
]

font = ImageFont.truetype(FONT_PATH, FONT_SIZE)
width_box = font.getbbox('A')
metrics_box = font.getbbox('Ag')
reference_width = width_box[2] - width_box[0]
origin_x = (FONT_WIDTH - reference_width) // 2 - width_box[0]
origin_y = 1 - metrics_box[1]


def rasterize(codepoint: int) -> list[int]:
    image = Image.new('L', (FONT_WIDTH, FONT_HEIGHT), 0)
    draw = ImageDraw.Draw(image)
    glyph = chr(codepoint) if codepoint >= 32 else ' '
    draw.text(
        (origin_x, origin_y),
        glyph,
        font=font,
        fill=255,
        stroke_width=0,
    )
    rows = []
    for y in range(FONT_HEIGHT):
        bits = 0
        for x in range(FONT_WIDTH):
            if image.getpixel((x, y)) >= THRESHOLD:
                bits |= 1 << x
        rows.append(bits)
    return rows


rows = {codepoint: rasterize(codepoint) for codepoint in CODEPOINTS}
output = Path('/home/ubuntu/npkernel/src/drivers/font.c')
with output.open('w', encoding='utf-8') as stream:
    stream.write('#include <stdint.h>\n#include <stddef.h>\n#include <npk/font.h>\n\n')
    stream.write(f'static const uint16_t ascii_font[128][{FONT_HEIGHT}] = {{\n')
    for codepoint in range(128):
        stream.write('    {' + ', '.join(f'0x{value:04x}' for value in rows[codepoint]) + '},\n')
    stream.write('};\n')
    stream.write(f'typedef struct {{ uint32_t cp; uint16_t rows[{FONT_HEIGHT}]; }} unicode_glyph_t;\n')
    stream.write('static const unicode_glyph_t unicode_font[] = {\n')
    for codepoint in CODEPOINTS[128:]:
        stream.write(
            '    { 0x%04x, {' % codepoint
            + ', '.join(f'0x{value:04x}' for value in rows[codepoint])
            + '} },\n'
        )
    stream.write('};\n')
    stream.write('const uint16_t *font_glyph(uint32_t cp) {\n')
    stream.write('    if (cp < 128) return ascii_font[cp];\n')
    stream.write('    for (size_t i = 0; i < sizeof(unicode_font) / sizeof(unicode_font[0]); ++i)\n')
    stream.write('        if (unicode_font[i].cp == cp) return unicode_font[i].rows;\n')
    stream.write("    return ascii_font[(uint8_t)'?'];\n")
    stream.write('}\n')
