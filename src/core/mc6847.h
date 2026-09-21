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

/* Which port A bit carries A/G is `docs/design.md` §16's medium-confidence
 * item, and the document states it two different ways: §2.3 lists bits 4-7
 * ascending as A/G, GM0, GM1, GM2, while §2.4 lists bits 7..4 descending
 * as A/G, GM2..GM0. Those disagree about where A/G sits. Until the Atom
 * circuit diagram settles it this is configuration, not a constant —
 * the same treatment §18 prescribes for the field rate. */
typedef enum {
    /* §2.4's reading: bit7=A/G, bit6=GM2, bit5=GM1, bit4=GM0. */
    VDG_BITS_AG_HIGH = 0,
    /* §2.3's reading: bit4=A/G, bit5=GM0, bit6=GM1, bit7=GM2. */
    VDG_BITS_AG_LOW  = 1
} vdg_bit_order_t;

/* Pack port A's top nibble and port C's CSS bit into the mode byte. */
uint8_t mc6847_pack_mode(uint8_t port_a_nibble, bool css, vdg_bit_order_t order);

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

typedef uint8_t mc6847_font_t[MC6847_FONT_GLYPHS][MC6847_FONT_ROWS];

/* ---- the VDG ---------------------------------------------------------- */

typedef struct {
    uint8_t mode;          /* packed; VDG_MODE_MASK                     */
    bool    lut_valid;
    uint8_t lut_px;        /* pixels per LUT entry for the current mode */

    /* One entry per byte value, already horizontally stretched (§8.3).
     * Worst case 16 pixels x 2 B x 256 = 8 KiB, which is ATOM_LUT_SIZE. */
    uint16_t lut[256u * (ATOM_LUT_ENTRY_MAX / 2u)];

    /* The 64-glyph character ROM. NULL until one is supplied; see
     * mc6847_set_font. */
    const uint8_t (*font)[MC6847_FONT_ROWS];
} mc6847_t;

void mc6847_init(mc6847_t *v);

/* Set the mode. Rebuilds the LUT only when the mode actually changes —
 * a few times per program run, not per field (§8.3). */
void mc6847_set_mode(mc6847_t *v, uint8_t mode);

/* Supply the character ROM. §16 requires this to be transcribed from the
 * datasheet and then verified by golden image, so the emulator does not
 * carry a guess: with no font set, alpha mode renders a placeholder that
 * is obviously wrong rather than subtly wrong. */
void mc6847_set_font(mc6847_t *v, const uint8_t (*font)[MC6847_FONT_ROWS]);
static inline bool mc6847_has_font(const mc6847_t *v) { return v->font != NULL; }

/* Generate one display row (0..191) as ATOM_SCREEN_W RGB565 pixels.
 * `vram` is the 6 KiB snapshot; `dst` holds at least 256 pixels. */
void mc6847_render_row(const mc6847_t *v, const uint8_t *vram,
                       unsigned y, uint16_t *dst);

#endif /* PICO_ATOM_MC6847_H */
