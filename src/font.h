#ifndef SYSDVR_FONT_H
#define SYSDVR_FONT_H
#include <stdint.h>
/* Bitmap font for the messages drawn on the display: ASCII and Cyrillic, one glyph is FONT_CELL_H rows of
 * 16-bit masks with the leftmost pixel in bit 15, FONT_CELL_W pixels wide (see tools/gen_font.py) */
extern const int FONT_CELL_W, FONT_CELL_H;
const uint16_t *font_glyph(uint32_t codepoint);     /* '?' for characters that are not in the font */
#endif
