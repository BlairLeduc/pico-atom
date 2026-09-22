/* vdg-ppm.c — render the VDG to PPM files, for eyeballing (design.md §16).
 *
 * §16's verification method for the character ROM is the honest one:
 * render the full 64-glyph set and compare it against the datasheet
 * figure, a photograph of a real Atom, or a reference emulator. This
 * tool produces the images to compare. It also renders a test pattern in
 * each of the nine modes, both colour sets, which is what §15.1's golden
 * images will be once there is something trustworthy to compare against.
 *
 *   vdg-ppm <outdir>
 *
 * Builds with the host target only; it is not part of the firmware.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "config.h"
#include "mc6847.h"

#if PICO_ATOM_HAVE_FONT
#include "mc6847_font.h"
#endif

static mc6847_t vdg;
static uint8_t  vram[ATOM_VRAM_SIZE];
static uint16_t row[ATOM_SCREEN_W];

static void write_ppm(const char *dir, const char *name) {
    char path[512];
    snprintf(path, sizeof(path), "%s/%s.ppm", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }

    fprintf(f, "P6\n%u %u\n255\n", ATOM_SCREEN_W, ATOM_SCREEN_H);
    for (unsigned y = 0; y < ATOM_SCREEN_H; y++) {
        mc6847_render_row(&vdg, vram, y, row);
        for (unsigned x = 0; x < ATOM_SCREEN_W; x++) {
            uint16_t p = row[x];
            /* RGB565 back to 8 bits per channel, replicating the high
             * bits so full-scale stays full-scale. */
            unsigned r = (p >> 11) & 0x1Fu, g = (p >> 5) & 0x3Fu, b = p & 0x1Fu;
            fputc((int)((r << 3) | (r >> 2)), f);
            fputc((int)((g << 2) | (g >> 4)), f);
            fputc((int)((b << 3) | (b >> 2)), f);
        }
    }
    fclose(f);
    printf("  %s.ppm\n", name);
}

static uint8_t mode_of(bool ag, unsigned gm, bool css) {
    return (uint8_t)((ag ? VDG_AG : 0u) |
                     ((gm << VDG_GM_SHIFT) & VDG_GM_MASK) |
                     (css ? VDG_CSS : 0u));
}

int main(int argc, char **argv) {
    const char *dir = (argc > 1) ? argv[1] : ".";

    mc6847_init(&vdg);
#if PICO_ATOM_HAVE_FONT
    mc6847_set_font(&vdg, mc6847_font);
    printf("character ROM: present\n");
#else
    printf("character ROM: ABSENT — alpha pages will show placeholder cells.\n"
           "Supply one with tools/mkfont.py to make the font sheets meaningful.\n");
#endif

    /* The font sheet: all 64 glyphs, normal then inverse, laid out 32 to
     * a line so the whole set fits on two pairs of character rows. */
    memset(vram, 0, sizeof(vram));
    for (unsigned g = 0; g < 64u; g++) {
        vram[(g / 32u) * 32u + (g % 32u)]                 = (uint8_t)g;
        vram[(2u + g / 32u) * 32u + (g % 32u)]            = (uint8_t)(g | 0x40u);
    }
    mc6847_set_mode(&vdg, mode_of(false, 0, false));
    write_ppm(dir, "font-sheet-css0");
    mc6847_set_mode(&vdg, mode_of(false, 0, true));
    write_ppm(dir, "font-sheet-css1");

    /* Every SG4 cell pattern: 16 quadrant combinations x 8 colours. */
    memset(vram, 0, sizeof(vram));
    for (unsigned c = 0; c < 8u; c++)
        for (unsigned q = 0; q < 16u; q++)
            vram[c * 32u + q] = (uint8_t)(0x80u | (c << 4) | q);
    mc6847_set_mode(&vdg, mode_of(false, 0, false));
    write_ppm(dir, "sg4-all-cells");

    /* A test pattern in each graphics mode, both colour sets. A ramp
     * makes a wrong stretch factor or a transposed colour obvious. */
    for (unsigned gm = 0; gm < 8u; gm++) {
        for (unsigned css = 0; css < 2u; css++) {
            uint8_t mode = mode_of(true, gm, css != 0);
            const mc6847_mode_info_t *info = mc6847_mode_info(mode);
            mc6847_set_mode(&vdg, mode);

            memset(vram, 0, sizeof(vram));
            for (unsigned r = 0; r < info->rows; r++)
                for (unsigned b = 0; b < info->bytes_per_row; b++)
                    vram[r * info->bytes_per_row + b] = (uint8_t)(r * 17u + b * 3u);

            char name[64];
            snprintf(name, sizeof(name), "%s-css%u", info->name, css);
            write_ppm(dir, name);
        }
    }

    return 0;
}
