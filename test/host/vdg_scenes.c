/* vdg_scenes.c — the fixed VRAM contents behind §15.1's golden images. */

#include "vdg_scenes.h"

#include <string.h>

_Static_assert(ATOM_SCREEN_W == 256 && ATOM_SCREEN_H == 192,
               "VDG_PPM_HEADER spells out the field size");

void vdg_render_rgb(const mc6847_t *v, const uint8_t *vram, uint8_t *rgb) {
    uint16_t row[ATOM_SCREEN_W];
    for (unsigned y = 0; y < ATOM_SCREEN_H; y++) {
        mc6847_render_row(v, vram, y, row);
        for (unsigned x = 0; x < ATOM_SCREEN_W; x++) {
            uint16_t p = row[x];
            unsigned r = (p >> 11) & 0x1Fu, g = (p >> 5) & 0x3Fu, b = p & 0x1Fu;
            *rgb++ = (uint8_t)((r << 3) | (r >> 2));
            *rgb++ = (uint8_t)((g << 2) | (g >> 4));
            *rgb++ = (uint8_t)((b << 3) | (b >> 2));
        }
    }
}

/* ---- alpha/SG4 --------------------------------------------------------- */

/* VRAM byte for an ASCII character in $20..$5F. The glyph index is the
 * low six bits, which is what makes the MC6847 order (index 0 is '@')
 * come out right without a table (mc6847.h). Bit 6 is inverse. */
static uint8_t screen_code(char c, bool inverse) {
    return (uint8_t)(((uint8_t)c & 0x3Fu) | (inverse ? 0x40u : 0u));
}

static void put_text(uint8_t *vram, unsigned row, unsigned col,
                     const char *s, bool inverse) {
    for (; *s && col < 32u; s++, col++)
        vram[row * 32u + col] = screen_code(*s, inverse);
}

/* A blank page is spaces, not zero: zero is '@' (mc6847.h). */
static void blank_page(uint8_t *vram) {
    memset(vram, 0, ATOM_VRAM_SIZE);
    memset(vram, MC6847_GLYPH_SPACE, 512u);
}

/* All 64 glyphs, normal then inverse, 32 to a line with a blank line
 * between the pairs. This is the sheet §16 wants compared against the
 * datasheet figure. */
static void fill_glyphs(uint8_t *vram, const mc6847_mode_info_t *info) {
    (void)info;
    blank_page(vram);
    for (unsigned g = 0; g < 64u; g++) {
        vram[(g / 32u) * 32u + (g % 32u)]      = (uint8_t)g;
        vram[(3u + g / 32u) * 32u + (g % 32u)] = (uint8_t)(g | 0x40u);
    }
}

/* §15.1's inverse-video text page: normal text, inverse text, the two
 * interleaved a character at a time, and SG4 blocks inline with text, as
 * a real Atom screen mixes them. */
static void fill_text(uint8_t *vram, const mc6847_mode_info_t *info) {
    (void)info;
    blank_page(vram);
    put_text(vram,  0, 0, "ACORN ATOM", false);
    put_text(vram,  2, 0, ">10 PRINT \"HELLO, WORLD\"", false);
    put_text(vram,  3, 0, ">20 GOTO 10", false);
    put_text(vram,  4, 0, ">RUN", false);
    put_text(vram,  5, 0, "HELLO, WORLD", false);
    put_text(vram,  7, 0, " INVERSE VIDEO 0123456789 ", true);
    put_text(vram,  8, 0, "!\"#$%&'()*+,-./:;<=>?@[\\]^_", true);

    const char *alt = "ALTERNATING NORMAL AND INVERSE";
    for (unsigned c = 0; alt[c]; c++)
        vram[10u * 32u + c] = screen_code(alt[c], (c & 1u) != 0);

    /* A text label boxed in by SG4 cells in every colour. */
    for (unsigned c = 0; c < 32u; c++) {
        vram[12u * 32u + c] = (uint8_t)(0x80u | ((c & 7u) << 4) | 0x3u);
        vram[14u * 32u + c] = (uint8_t)(0x80u | ((c & 7u) << 4) | 0xCu);
    }
    put_text(vram, 13, 0, "SG4 ABOVE AND BELOW THIS LINE", false);

    put_text(vram, 15, 0, ">", false);
    vram[15u * 32u + 1u] = screen_code(' ', true);   /* a block cursor */
}

/* Every SG4 cell: the 16 quadrant patterns across, the 8 colours down,
 * each cell separated by a blank one so a quadrant bleeding into its
 * neighbour shows. */
static void fill_sg4(uint8_t *vram, const mc6847_mode_info_t *info) {
    (void)info;
    blank_page(vram);
    for (unsigned c = 0; c < 8u; c++)
        for (unsigned q = 0; q < 16u; q++)
            vram[(c * 2u) * 32u + q * 2u] = (uint8_t)(0x80u | (c << 4) | q);
}

/* ---- graphics ---------------------------------------------------------- */

static unsigned bits_per_px(const mc6847_mode_info_t *info) {
    return info->kind == VDG_CG ? 2u : 1u;
}

static unsigned src_width(const mc6847_mode_info_t *info) {
    return info->bytes_per_row * (8u / bits_per_px(info));
}

/* Set one source pixel to colour index c (0..3 in CG, 0..1 in RG),
 * most significant bits leftmost as the part shifts them out. */
static void plot(uint8_t *vram, const mc6847_mode_info_t *info,
                 unsigned x, unsigned y, unsigned c) {
    unsigned bpp   = bits_per_px(info);
    unsigned per   = 8u / bpp;
    unsigned shift = (per - 1u - x % per) * bpp;
    uint8_t  mask  = (uint8_t)(((1u << bpp) - 1u) << shift);
    uint8_t *b     = &vram[y * info->bytes_per_row + x / per];
    *b = (uint8_t)((*b & ~mask) | ((c << shift) & mask));
}

/* Top half: one bar per colour index, left to right in palette order.
 * Bottom half, CG: the byte 0x1B (indices 0,1,2,3) repeated, which
 * catches reversed pixel order within a byte; RG: a one-pixel
 * checkerboard, which catches a wrong horizontal or vertical stretch.
 * Over both: a border on the outermost source pixels and a diagonal from
 * corner to corner, in the last colour. */
static void fill_graphics(uint8_t *vram, const mc6847_mode_info_t *info) {
    unsigned w = src_width(info), h = info->rows;
    unsigned colours = 1u << bits_per_px(info);
    unsigned ink = colours - 1u;

    memset(vram, 0, ATOM_VRAM_SIZE);
    for (unsigned y = 0; y < h; y++) {
        for (unsigned x = 0; x < w; x++) {
            unsigned c;
            if (y < h / 2u)                c = x * colours / w;
            else if (info->kind == VDG_CG) c = x & 3u;
            else                           c = (x ^ y) & 1u;
            plot(vram, info, x, y, c);
        }
    }

    for (unsigned x = 0; x < w; x++) {
        plot(vram, info, x, 0, ink);
        plot(vram, info, x, h - 1u, ink);
    }
    for (unsigned y = 0; y < h; y++) {
        plot(vram, info, 0, y, ink);
        plot(vram, info, w - 1u, y, ink);
        plot(vram, info, y * (w - 1u) / (h - 1u), y, ink);
    }
}

/* ---- the list ---------------------------------------------------------- */

#define GFX(gm) ((uint8_t)(VDG_AG | ((gm) << VDG_GM_SHIFT)))

const vdg_scene_t vdg_scenes[] = {
    { "alpha-glyphs", 0,       true,  fill_glyphs   },
    { "alpha-text",   0,       true,  fill_text     },
    { "alpha-sg4",    0,       false, fill_sg4      },
    { "cg1",          GFX(0u), false, fill_graphics },
    { "rg1",          GFX(1u), false, fill_graphics },
    { "cg2",          GFX(2u), false, fill_graphics },
    { "rg2",          GFX(3u), false, fill_graphics },
    { "cg3",          GFX(4u), false, fill_graphics },
    { "rg3",          GFX(5u), false, fill_graphics },
    { "cg6",          GFX(6u), false, fill_graphics },
    { "rg6",          GFX(7u), false, fill_graphics },
};

const unsigned vdg_scene_count = sizeof(vdg_scenes) / sizeof(vdg_scenes[0]);
