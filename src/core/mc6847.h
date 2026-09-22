/* mc6847.h — MC6847 VDG (design.md §8).
 *
 * There is no decoded framebuffer, by design (§8.1): the image is a pure
 * function of 6 KiB of guest VRAM plus five mode bits, so rows are
 * generated straight from a snapshot through an expansion LUT into the
 * RGB565 line buffers. Adding a framebuffer would reintroduce the second
 * copy of the screen this design exists to avoid.
 */
#ifndef PICO_ATOM_MC6847_H
#define PICO_ATOM_MC6847_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* ---- the five mode bits, packed (§8.1) ------------------------------ */

#define VDG_CSS       0x01u   /* colour set select, from port C bit 3 */
#define VDG_GM_MASK   0x0Eu   /* GM2:GM0 in bits 3:1                  */
#define VDG_GM_SHIFT  1u
#define VDG_AG        0x10u   /* alpha (0) or graphics (1)            */
#define VDG_MODE_MASK 0x1Fu

/* Port A's mode nibble, read off the Atom circuit diagram:
 *
 *     bit 4 = A/G, bit 5 = GM0, bit 6 = GM1, bit 7 = GM2
 *
 * This settles §16's medium-confidence row and the contradiction between
 * design.md §2.3 (which had it right) and §2.4 (which did not). It was
 * configuration while it was unverified; now that the schematic has
 * spoken it is a constant, so there is nothing left to pick wrong. */
#define VDG_PORT_A_AG_BIT   4u
#define VDG_PORT_A_GM0_BIT  5u
#define VDG_PORT_A_GM1_BIT  6u
#define VDG_PORT_A_GM2_BIT  7u

/* Pack port A's top nibble and port C's CSS bit into the mode byte. */
uint8_t mc6847_pack_mode(uint8_t port_a_nibble, bool css);

/* ---- modes (§2.4, §8.3) --------------------------------------------- */

typedef enum { VDG_ALPHA, VDG_CG, VDG_RG } vdg_kind_t;

typedef struct {
    vdg_kind_t  kind;
    const char *name;
    uint16_t    bytes_per_row;  /* VRAM bytes in one source row        */
    uint16_t    rows;           /* source rows                         */
    uint8_t     x_scale;        /* horizontal stretch                  */
    uint8_t     y_scale;        /* vertical stretch (a repeat count)   */
    uint8_t     px_per_byte;    /* screen pixels one source byte covers */
    uint16_t    vram_bytes;
} mc6847_mode_info_t;

/* Valid for any mode byte; alpha ignores GM2:0. */
const mc6847_mode_info_t *mc6847_mode_info(uint8_t mode);

/* ---- palette (§2.4) -------------------------------------------------- */

typedef enum {
    VDG_GREEN, VDG_YELLOW, VDG_BLUE, VDG_RED,
    VDG_BUFF, VDG_CYAN, VDG_MAGENTA, VDG_ORANGE,
    VDG_BLACK,
    VDG_COLOUR_COUNT
} vdg_colour_t;

extern const uint16_t mc6847_palette[VDG_COLOUR_COUNT];

/* ---- the character ROM ---------------------------------------------- */

#define MC6847_FONT_GLYPHS ATOM_FONT_GLYPHS   /* 64 */
#define MC6847_FONT_ROWS   ATOM_FONT_ROWS     /* 12 */
#define MC6847_FONT_BYTES  (MC6847_FONT_GLYPHS * MC6847_FONT_ROWS)   /* 768 */

/* The ROM is a flat array: glyph g row r is font[g * 12 + r].
 *
 * A glyph is 5 pixels wide and sits in *bits 5..1* of its byte, which is
 * how the part's own ROM is laid out — not bit 7 leftmost. The renderer
 * shifts it into the 8-wide cell, leaving the three spacing columns on
 * the right. Bits 7, 6 and 0 are unused by the data.
 *
 * Vertically the 7 glyph rows occupy rows 3..9 of the 12-row cell; that
 * is carried by the data itself and needs no handling here.
 *
 * Glyph order is the MC6847's internal one, which is *not* plain ASCII:
 *
 *     index  0..31  ->  ASCII $40..$5F   (@ A..Z [ \ ] ^ _)
 *     index 32..63  ->  ASCII $20..$3F   (space ! " ... ?)
 *
 * so ascii = 0x20 + ((index + 0x20) & 0x3F). The Atom's VRAM byte
 * supplies the index directly in bits 5..0, so nothing translates. */
#define MC6847_FONT_GLYPH_W  5u
#define MC6847_FONT_LEFT_BIT 5u
#define MC6847_FONT_LSHIFT   (7u - MC6847_FONT_LEFT_BIT)

/* Glyph 0 is '@', not the space; a page of zeroed VRAM is a page of
 * '@'. The space is glyph 32. */
#define MC6847_GLYPH_SPACE 32u

/* ASCII code of a glyph index, for tooling and diagnostics. */
static inline uint8_t mc6847_glyph_ascii(uint8_t index) {
    return (uint8_t)(0x20u + ((index + 0x20u) & 0x3Fu));
}

/* ---- the VDG ---------------------------------------------------------- */

typedef struct {
    uint8_t mode;          /* packed; VDG_MODE_MASK                     */
    bool    lut_valid;
    uint8_t lut_px;        /* pixels per LUT entry for the current mode */

    /* One entry per byte value, already horizontally stretched (§8.3).
     * Worst case 16 pixels x 2 B x 256 = 8 KiB, which is ATOM_LUT_SIZE. */
    uint16_t lut[256u * (ATOM_LUT_ENTRY_MAX / 2u)];

    /* The 64-glyph character ROM, MC6847_FONT_BYTES long. NULL until one
     * is supplied; see mc6847_set_font. */
    const uint8_t *font;
} mc6847_t;

void mc6847_init(mc6847_t *v);

/* Set the mode. Rebuilds the LUT only when the mode actually changes —
 * a few times per program run, not per field (§8.3). */
void mc6847_set_mode(mc6847_t *v, uint8_t mode);

/* Supply the character ROM. §16 requires this to be transcribed from the
 * datasheet and then verified by golden image, so the emulator does not
 * carry a guess: with no font set, alpha mode renders a placeholder that
 * is obviously wrong rather than subtly wrong. */
void mc6847_set_font(mc6847_t *v, const uint8_t *font);
static inline bool mc6847_has_font(const mc6847_t *v) { return v->font != NULL; }

/* Generate one display row (0..191) as ATOM_SCREEN_W RGB565 pixels.
 * `vram` is the 6 KiB snapshot; `dst` holds at least 256 pixels. */
void mc6847_render_row(const mc6847_t *v, const uint8_t *vram,
                       unsigned y, uint16_t *dst);

#endif /* PICO_ATOM_MC6847_H */
