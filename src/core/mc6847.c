/* mc6847.c — MC6847 mode decode, expansion LUT and row generation
 * (design.md §8.3).
 */

#include "mc6847.h"

#include <string.h>

#define RGB565(r, g, b) \
    ((uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3)))

/* The nine colours of §2.4. The *set* is the datasheet's; the exact
 * analogue values a real VDG puts on a composite output are not, so these
 * are the idealised primaries and are a rendering choice. They are the
 * one thing here a golden image will happily bless without proving. */
const uint16_t mc6847_palette[VDG_COLOUR_COUNT] = {
    [VDG_GREEN]   = RGB565(0x00, 0xFF, 0x00),
    [VDG_YELLOW]  = RGB565(0xFF, 0xFF, 0x00),
    [VDG_BLUE]    = RGB565(0x00, 0x00, 0xFF),
    [VDG_RED]     = RGB565(0xFF, 0x00, 0x00),
    [VDG_BUFF]    = RGB565(0xFF, 0xFF, 0xFF),
    [VDG_CYAN]    = RGB565(0x00, 0xFF, 0xFF),
    [VDG_MAGENTA] = RGB565(0xFF, 0x00, 0xFF),
    [VDG_ORANGE]  = RGB565(0xFF, 0x80, 0x00),
    [VDG_BLACK]   = RGB565(0x00, 0x00, 0x00),
};

/* CG modes: four colours, selected by CSS (§2.4). */
static const uint8_t cg_set[2][4] = {
    { VDG_GREEN, VDG_YELLOW,  VDG_BLUE,    VDG_RED    },   /* CSS = 0 */
    { VDG_BUFF,  VDG_CYAN,    VDG_MAGENTA, VDG_ORANGE },   /* CSS = 1 */
};

/* RG modes: black plus one. */
static const uint8_t rg_fg[2] = { VDG_GREEN, VDG_BUFF };

uint8_t mc6847_pack_mode(uint8_t port_a_nibble, bool css) {
    /* bit 4 = A/G, bit 5 = GM0, bit 6 = GM1, bit 7 = GM2 (schematic).
     * The GM bits ascend as the bit number rises, so they come out
     * reversed relative to the GM2:GM1:GM0 value the mode table uses. */
    uint8_t ag = (uint8_t)((port_a_nibble >> VDG_PORT_A_AG_BIT) & 1u);
    uint8_t gm = (uint8_t)((((port_a_nibble >> VDG_PORT_A_GM2_BIT) & 1u) << 2) |
                           (((port_a_nibble >> VDG_PORT_A_GM1_BIT) & 1u) << 1) |
                           (((port_a_nibble >> VDG_PORT_A_GM0_BIT) & 1u) << 0));
    return (uint8_t)((ag ? VDG_AG : 0u) |
                     ((gm << VDG_GM_SHIFT) & VDG_GM_MASK) |
                     (css ? VDG_CSS : 0u));
}

/* §2.4's mode table and §8.3's stretch factors, as one set of numbers.
 * The invariants that tie them together — bytes_per_row * px_per_byte ==
 * 256, rows * y_scale == 192, bytes_per_row * rows == vram_bytes — are
 * asserted by test_mc6847.c rather than trusted here. */
static const mc6847_mode_info_t modes[9] = {
    /* index 0 is alpha/SG6; 1..8 are GM2:0 + 1 */
    { VDG_ALPHA, "Alpha/SG6", 32,  16, 8, 12, 8,  512  },
    { VDG_CG,    "CG1",       16,  64, 4,  3, 16, 1024 },
    { VDG_RG,    "RG1",       16,  64, 2,  3, 16, 1024 },
    { VDG_CG,    "CG2",       32,  64, 2,  3, 8,  2048 },
    { VDG_RG,    "RG2",       16,  96, 2,  2, 16, 1536 },
    { VDG_CG,    "CG3",       32,  96, 2,  2, 8,  3072 },
    { VDG_RG,    "RG3",       16, 192, 2,  1, 16, 3072 },
    { VDG_CG,    "CG6",       32, 192, 2,  1, 8,  6144 },
    { VDG_RG,    "RG6",       32, 192, 1,  1, 8,  6144 },
};

const mc6847_mode_info_t *mc6847_mode_info(uint8_t mode) {
    if (!(mode & VDG_AG)) return &modes[0];
    return &modes[1u + ((mode & VDG_GM_MASK) >> VDG_GM_SHIFT)];
}

void mc6847_init(mc6847_t *v) {
    memset(v, 0, sizeof(*v));
    v->font = NULL;
    v->mode = 0xFFu;             /* force the first set_mode to build */
    mc6847_set_mode(v, 0);
}

void mc6847_set_font(mc6847_t *v, const uint8_t *font) {
    v->font = font;
}

/* Build the expansion LUT: one entry per byte value, already stretched
 * horizontally, so a row becomes a tight run of fixed-size copies. */
static void build_lut(mc6847_t *v) {
    const mc6847_mode_info_t *info = mc6847_mode_info(v->mode);
    bool css = (v->mode & VDG_CSS) != 0;

    if (info->kind == VDG_ALPHA) {
        /* Alpha and SG6 are generated per character cell, not through a
         * byte LUT, because a cell's appearance depends on the row
         * within it as well as on the byte. */
        v->lut_px = 0;
        v->lut_valid = true;
        return;
    }

    v->lut_px = info->px_per_byte;

    for (unsigned b = 0; b < 256u; b++) {
        uint16_t *e = &v->lut[b * info->px_per_byte];
        unsigned n = 0;
        if (info->kind == VDG_CG) {
            /* Four pixels, two bits each, most significant pair first. */
            for (int p = 3; p >= 0; p--) {
                unsigned idx = (b >> (p * 2)) & 3u;
                uint16_t c = mc6847_palette[cg_set[css ? 1 : 0][idx]];
                for (unsigned s = 0; s < info->x_scale; s++) e[n++] = c;
            }
        } else {
            /* Eight pixels, one bit each, most significant first. */
            uint16_t fg = mc6847_palette[rg_fg[css ? 1 : 0]];
            uint16_t bg = mc6847_palette[VDG_BLACK];
            for (int p = 7; p >= 0; p--) {
                uint16_t c = ((b >> p) & 1u) ? fg : bg;
                for (unsigned s = 0; s < info->x_scale; s++) e[n++] = c;
            }
        }
    }
    v->lut_valid = true;
}

void mc6847_set_mode(mc6847_t *v, uint8_t mode) {
    mode &= VDG_MODE_MASK;
    if (v->lut_valid && mode == v->mode) return;   /* a few times per run */
    v->mode = mode;
    v->lut_valid = false;
    build_lut(v);
}

/* ---- alpha and semigraphics (§2.4) ---------------------------------- */

/* SG6 elements, two across and three down, each 4 px by 4 rows: bits 5
 * and 4 are the top pair, 3 and 2 the middle, 1 and 0 the bottom, left
 * then right. The colour is C1:C0 = D7:D6 in CSS's set (MC6847 data
 * sheet), and since the Atom's D6 is what made this a graphics cell, only
 * the odd colours are reachable: yellow and red, or cyan and orange. */
static void render_sg6_cell(uint8_t byte, unsigned row_in_cell, bool css,
                            uint16_t *dst) {
    uint16_t colour = mc6847_palette[(css ? 4u : 0u) | (byte >> 6)];
    uint16_t black  = mc6847_palette[VDG_BLACK];

    unsigned pair = row_in_cell / (MC6847_FONT_ROWS / 3u);   /* 0, 1, 2 */
    unsigned left_bit = 5u - 2u * pair;

    uint16_t l = ((byte >> left_bit) & 1u) ? colour : black;
    uint16_t r = ((byte >> (left_bit - 1u)) & 1u) ? colour : black;

    for (unsigned x = 0; x < 4u; x++) dst[x] = l;
    for (unsigned x = 4; x < 8u; x++) dst[x] = r;
}

static void render_alpha_cell(const mc6847_t *v, uint8_t byte,
                              unsigned row_in_cell, bool css, uint16_t *dst) {
    bool inverse = (byte & VDG_BYTE_INV) != 0;
    uint16_t fg = mc6847_palette[css ? VDG_ORANGE : VDG_GREEN];
    uint16_t bg = mc6847_palette[VDG_BLACK];
    if (inverse) { uint16_t t = fg; fg = bg; bg = t; }

    uint8_t bits;
    if (v->font) {
        /* The glyph is 5 wide in bits 5..1 of the ROM byte; shift it to
         * bit 7 leftmost, which leaves the three spacing columns on the
         * right of the 8-wide cell. */
        bits = (uint8_t)(v->font[(byte & 0x3Fu) * MC6847_FONT_ROWS + row_in_cell]
                         << MC6847_FONT_LSHIFT);
    } else {
        /* No character ROM supplied. Draw a hollow box for every glyph:
         * unmistakably wrong on sight, which is the point — a plausible
         * guessed font would be wrong in a way nobody notices (§16). */
        bits = (row_in_cell == 0 || row_in_cell == MC6847_FONT_ROWS - 1u)
                   ? 0xFEu : 0x82u;
    }

    for (unsigned x = 0; x < 8u; x++)
        dst[x] = ((bits >> (7u - x)) & 1u) ? fg : bg;
}

/* ---- row generation -------------------------------------------------- */

void mc6847_render_row(const mc6847_t *v, const uint8_t *vram,
                       unsigned y, uint16_t *dst) {
    const mc6847_mode_info_t *info = mc6847_mode_info(v->mode);

    if (info->kind == VDG_ALPHA) {
        unsigned cell_row    = y / MC6847_FONT_ROWS;
        unsigned row_in_cell = y % MC6847_FONT_ROWS;
        const uint8_t *p = vram + cell_row * info->bytes_per_row;
        bool css = (v->mode & VDG_CSS) != 0;

        for (unsigned c = 0; c < info->bytes_per_row; c++) {
            uint8_t byte = p[c];
            uint16_t *cell = dst + c * 8u;
            /* Bit 6 selects semigraphics 6; this is where the Atom's
             * chunky block graphics come from, and why plotting in
             * CLEAR 0 works at all (§2.4). */
            if (byte & VDG_BYTE_SG6) render_sg6_cell(byte, row_in_cell, css, cell);
            else                     render_alpha_cell(v, byte, row_in_cell, css, cell);
        }
        return;
    }

    /* Vertical stretch is free: y_scale is a repeat count, so a repeated
     * row re-reads the same source bytes (§8.3). */
    unsigned src_row = y / info->y_scale;
    const uint8_t *p = vram + src_row * info->bytes_per_row;

    /* Unrolled by entry size, not memcpy: at 16 or 32 bytes the call is
     * the cost (hardware-notes.md §9.4). */
    if (info->px_per_byte == 8u) {
        for (unsigned b = 0; b < info->bytes_per_row; b++) {
            const uint16_t *e = &v->lut[p[b] * 8u];
            uint16_t *d = dst + b * 8u;
            d[0] = e[0]; d[1] = e[1]; d[2] = e[2]; d[3] = e[3];
            d[4] = e[4]; d[5] = e[5]; d[6] = e[6]; d[7] = e[7];
        }
    } else {
        for (unsigned b = 0; b < info->bytes_per_row; b++) {
            const uint16_t *e = &v->lut[p[b] * 16u];
            uint16_t *d = dst + b * 16u;
            for (unsigned i = 0; i < 16u; i++) d[i] = e[i];
        }
    }
}

/* ---- dirty bands (design.md §8.4) ------------------------------------ */

/* The source rows a band of display rows reads, from the same geometry
 * mc6847_render_row uses — so the diff and the renderer cannot disagree
 * about which bytes a band depends on. */
static void band_source_rows(const mc6847_mode_info_t *info, unsigned band,
                             unsigned *first, unsigned *last) {
    unsigned y0 = band * ATOM_BAND_ROWS;
    unsigned y1 = y0 + ATOM_BAND_ROWS - 1u;
    unsigned per = (info->kind == VDG_ALPHA) ? MC6847_FONT_ROWS : info->y_scale;
    *first = y0 / per;
    *last  = y1 / per;
}

bool mc6847_band_span(uint8_t mode, const uint8_t *vram, const uint8_t *shadow,
                      unsigned band, uint16_t *x0, uint16_t *x1) {
    const mc6847_mode_info_t *info = mc6847_mode_info(mode);
    unsigned first, last;
    band_source_rows(info, band, &first, &last);

    unsigned lo = info->bytes_per_row, hi = 0;
    for (unsigned r = first; r <= last; r++) {
        const uint8_t *a = vram   + r * info->bytes_per_row;
        const uint8_t *b = shadow + r * info->bytes_per_row;
        for (unsigned c = 0; c < info->bytes_per_row; c++) {
            if (a[c] != b[c]) {
                if (c < lo) lo = c;
                if (c > hi) hi = c;
            }
        }
    }
    if (lo > hi) return false;

    *x0 = (uint16_t)(lo * info->px_per_byte);
    *x1 = (uint16_t)((hi + 1u) * info->px_per_byte - 1u);
    return true;
}

unsigned mc6847_row_source(uint8_t mode, unsigned y) {
    const mc6847_mode_info_t *info = mc6847_mode_info(mode);
    /* Alpha rows are all distinct — each is a different glyph row — so
     * only graphics modes have repeats to reuse. */
    if (info->kind == VDG_ALPHA) return y;
    return y / info->y_scale;
}
