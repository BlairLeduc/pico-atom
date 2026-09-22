/* vdg_scenes.h — the fixed VRAM contents behind §15.1's golden images.
 *
 * One list, shared by tools/vdg-ppm (which writes the images) and
 * test_mc6847_golden (which compares against them), so the two cannot
 * drift apart. Each scene is rendered once per colour set.
 *
 * The graphics scenes are built to be checked by eye, not just
 * regression-compared: a border on the outermost source pixels proves
 * the extents, a corner-to-corner diagonal proves the stretch factors,
 * and colour bars prove the palette order. A golden image only proves
 * anything once someone has looked at it.
 */
#ifndef PICO_ATOM_VDG_SCENES_H
#define PICO_ATOM_VDG_SCENES_H

#include <stdbool.h>
#include <stdint.h>

#include "config.h"
#include "mc6847.h"

typedef struct {
    const char *name;                 /* file stem; "-cssN" is appended */
    uint8_t     mode;                 /* packed, CSS clear              */
    bool        needs_font;           /* meaningless without the ROM    */
    void      (*fill)(uint8_t *vram, const mc6847_mode_info_t *info);
} vdg_scene_t;

extern const vdg_scene_t vdg_scenes[];
extern const unsigned    vdg_scene_count;

#define VDG_PPM_BYTES (ATOM_SCREEN_W * ATOM_SCREEN_H * 3u)

/* Render a whole field as 8-bit RGB, RGB565 widened by replicating the
 * high bits so full scale stays full scale. */
void vdg_render_rgb(const mc6847_t *v, const uint8_t *vram, uint8_t *rgb);

/* The PPM (P6) header for a field; the pixel data follows it. */
#define VDG_PPM_HEADER "P6\n256 192\n255\n"

#endif /* PICO_ATOM_VDG_SCENES_H */
