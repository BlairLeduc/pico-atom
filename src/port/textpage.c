/* textpage.c — a page of text in alpha-mode VRAM (textpage.h). */

#include "textpage.h"

#include <string.h>

#include "mc6847.h"

/* ASCII to the MC6847's glyph order (CLAUDE.md): #40-#5F are glyphs
 * 0-31, #20-#3F are 32-63. Anything else is a blank. */
static uint8_t glyph(char c, bool inverse) {
    uint8_t a = (uint8_t)c;
    if (a >= 'a' && a <= 'z') a = (uint8_t)(a - 32);
    uint8_t g = (a >= 0x40u && a < 0x60u) ? (uint8_t)(a - 0x40u)
              : (a >= 0x20u && a < 0x40u) ? a : (uint8_t)' ';
    return inverse ? (uint8_t)(g | VDG_BYTE_INV) : g;
}

void textpage_clear(uint8_t *vram) {
    memset(vram, glyph(' ', false), TEXT_COLS * TEXT_ROWS);
}

void textpage_put(uint8_t *vram, int row, int col, const char *s, bool inverse) {
    if (row < 0 || row >= TEXT_ROWS) return;
    for (; *s && col < TEXT_COLS; s++, col++) {
        if (col >= 0) vram[row * TEXT_COLS + col] = glyph(*s, inverse);
    }
}

void textpage_line(uint8_t *vram, int row, const char *s, bool inverse) {
    if (row < 0 || row >= TEXT_ROWS) return;
    for (int col = 0; col < TEXT_COLS; col++) {
        char c = *s ? *s++ : ' ';
        vram[row * TEXT_COLS + col] = glyph(c, inverse);
    }
}
