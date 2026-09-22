/* test_mc6847_golden.c — §15.1's golden images: fixed VRAM in all nine
 * modes, both colour sets, compared against committed PPMs in
 * test/golden/. This is M2's "done when" (design.md §17).
 *
 * The scenes live in vdg_scenes.c, shared with tools/vdg-ppm, which is
 * what writes the goldens. On a mismatch the rendered image is written
 * beside the test binary as <name>.actual.ppm, to compare by eye.
 *
 * Without the character ROM, the scenes that need it are skipped rather
 * than compared: the placeholder cells are not what the goldens show.
 */

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "mc6847.h"
#if PICO_ATOM_HAVE_FONT
#include "mc6847_font.h"
#endif
#include "test_util.h"
#include "vdg_scenes.h"

static mc6847_t g_vdg;
static uint8_t  g_vram[ATOM_VRAM_SIZE];
static uint8_t  g_got[VDG_PPM_BYTES];
static uint8_t  g_want[VDG_PPM_BYTES];

/* Read a golden into g_want. False if missing or not a 256x192 P6. */
static bool load_golden(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return false;
    char header[sizeof(VDG_PPM_HEADER) - 1u];
    bool ok = fread(header, 1, sizeof(header), f) == sizeof(header) &&
              memcmp(header, VDG_PPM_HEADER, sizeof(header)) == 0 &&
              fread(g_want, 1, sizeof(g_want), f) == sizeof(g_want);
    fclose(f);
    return ok;
}

static void save_actual(const char *name) {
    char path[256];
    snprintf(path, sizeof(path), "%s.actual.ppm", name);
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); return; }
    fputs(VDG_PPM_HEADER, f);
    fwrite(g_got, 1, sizeof(g_got), f);
    fclose(f);
    fprintf(stderr, "    rendered image written to %s\n", path);
}

int main(void) {
    mc6847_init(&g_vdg);
#if PICO_ATOM_HAVE_FONT
    mc6847_set_font(&g_vdg, font_6847);
#endif

    unsigned compared = 0, skipped = 0;

    for (unsigned i = 0; i < vdg_scene_count; i++) {
        const vdg_scene_t *s = &vdg_scenes[i];
        if (s->needs_font && !mc6847_has_font(&g_vdg)) {
            printf("skip %s: no character ROM\n", s->name);
            skipped++;
            continue;
        }

        for (unsigned css = 0; css < 2u; css++) {
            uint8_t mode = (uint8_t)(s->mode | (css ? VDG_CSS : 0u));
            mc6847_set_mode(&g_vdg, mode);
            s->fill(g_vram, mc6847_mode_info(mode));
            vdg_render_rgb(&g_vdg, g_vram, g_got);

            char name[64], path[512];
            snprintf(name, sizeof(name), "%s-css%u", s->name, css);
            snprintf(path, sizeof(path), "%s/%s.ppm", PICO_ATOM_GOLDEN_DIR, name);

            if (!load_golden(path)) {
                CHECK(false, "%s: missing or malformed; regenerate with "
                      "vdg-ppm test/golden and look at it first", path);
                continue;
            }

            if (memcmp(g_got, g_want, sizeof(g_got)) != 0) {
                size_t at = 0, diff = 0;
                for (size_t p = 0; p < VDG_PPM_BYTES; p += 3u) {
                    if (memcmp(&g_got[p], &g_want[p], 3u) != 0) {
                        if (!diff) at = p / 3u;
                        diff++;
                    }
                }
                CHECK(false, "%s: %zu pixel(s) differ, first at (%zu,%zu)",
                      name, diff, at % ATOM_SCREEN_W, at / ATOM_SCREEN_W);
                save_actual(name);
            }
            compared++;
        }
    }

    printf("%u image(s) compared, %u scene(s) skipped\n", compared, skipped);
    TEST_DONE();
}
