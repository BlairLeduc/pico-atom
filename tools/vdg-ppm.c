/* vdg-ppm.c — render the VDG to PPM files (design.md §15.1, §16).
 *
 * Renders every scene in test/host/vdg_scenes.c, both colour sets. Two
 * uses:
 *
 *   vdg-ppm <outdir>        eyeball the output, e.g. the glyph sheet
 *                           against the datasheet figure (§16)
 *   vdg-ppm test/golden     regenerate §15.1's golden images, which
 *                           test_mc6847_golden then compares against
 *
 * Regenerating the goldens blesses whatever the renderer does now. Look
 * at every image that changed before committing it.
 *
 * Builds with the host target only; it is not part of the firmware.
 */

#include <stdio.h>
#include <stdlib.h>

#include "config.h"
#include "mc6847.h"
#include "vdg_scenes.h"

#if PICO_ATOM_HAVE_FONT
#include "mc6847_font.h"
#endif

static mc6847_t vdg;
static uint8_t  vram[ATOM_VRAM_SIZE];
static uint8_t  rgb[VDG_PPM_BYTES];

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";

    mc6847_init(&vdg);
#if PICO_ATOM_HAVE_FONT
    mc6847_set_font(&vdg, font_6847);
    printf("character ROM: present\n");
#else
    printf("character ROM: ABSENT — alpha text scenes will show placeholder\n"
           "cells and are not written.\n");
#endif

    for (unsigned i = 0; i < vdg_scene_count; i++) {
        const vdg_scene_t *s = &vdg_scenes[i];
        if (s->needs_font && !mc6847_has_font(&vdg)) continue;

        for (unsigned css = 0; css < 2u; css++) {
            uint8_t mode = (uint8_t)(s->mode | (css ? VDG_CSS : 0u));
            mc6847_set_mode(&vdg, mode);
            s->fill(vram, mc6847_mode_info(mode));
            vdg_render_rgb(&vdg, vram, rgb);

            char path[512];
            snprintf(path, sizeof(path), "%s/%s-css%u.ppm", dir, s->name, css);
            FILE *f = fopen(path, "wb");
            if (!f) { perror(path); return 1; }
            fputs(VDG_PPM_HEADER, f);
            fwrite(rgb, 1, sizeof(rgb), f);
            fclose(f);
            printf("  %s\n", path);
        }
    }
    return 0;
}
