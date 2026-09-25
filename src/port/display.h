/* display.h — snapshot diff and band present (design.md §8.2-8.4).
 *
 * Core 1 only. Owns the renderer (and so the only mode-expansion LUT),
 * the presented shadow and the DMA line buffers. There is no decoded
 * framebuffer (§8.1): every pixel sent is generated from a snapshot.
 */
#ifndef PICO_ATOM_DISPLAY_H
#define PICO_ATOM_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    uint32_t us;        /* wall time of the present                     */
    uint16_t bands;     /* bands sent (24 for a full redraw)            */
    uint32_t pixels;    /* pixels on the wire                           */
    bool     full;      /* whole-rectangle redraw: mode change or forced */
    bool     border;    /* the border was filled too                     */
} display_stats_t;

void display_init(const uint8_t *font);

/* Colour or monochrome, and whether the VDG's border is drawn as a
 * frame around the Atom's rectangle, ATOM_BORDER_X by ATOM_BORDER_Y
 * (design.md §8.7). A change repaints
 * everything at the next present. With the border off the panel around
 * the rectangle is black, as it was before M10. */
void display_set_look(bool mono, bool border);

/* Present one snapshot. If its mode byte differs from the presented one,
 * the whole rectangle is redrawn, and the border too if its colour has
 * changed — a mode or CSS change leaves VRAM
 * byte-identical, so a VRAM-only diff would find nothing (§8.4 step 1).
 * Otherwise only dirty band spans are sent. The snapshot and its mode
 * then become the shadow. */
void display_present(const uint8_t *vram, uint8_t mode, display_stats_t *st);

/* Forget what is on the panel, so the next present redraws everything.
 * This is how an overlay is dismissed (design.md §13), and how the
 * measurement forces full redraws. */
void display_invalidate(void);

/* M3's test pattern (design.md §17): a 1-px white border exactly on the
 * 256x192 rectangle at (32,64) with a 16x16 block in each inside corner —
 * red top-left, green top-right, blue bottom-left, yellow bottom-right —
 * so a mirrored axis or swapped R/B shows as the wrong colour in the
 * wrong corner. Everything outside the rectangle stays black. Also
 * invalidates, since it overwrites the Atom rectangle. */
void display_test_pattern(void);

#endif /* PICO_ATOM_DISPLAY_H */
