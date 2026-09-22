/* test_present.c — dirty bands and the snapshot pool (design.md §4.2, §8.4).
 *
 * The band diff is checked by executing it: a simulated panel is brought
 * up to date by drawing only the spans mc6847_band_span reports, and must
 * then match a full render of the same VRAM. A span that is too narrow,
 * or a band it forgot, leaves a stale pixel and fails here — which is
 * the "stale sprite on screen for ever" failure the design warns about.
 */

#include <stdlib.h>
#include <string.h>

#include "mc6847.h"
#include "snappool.h"
#include "test_util.h"

#if PICO_ATOM_HAVE_FONT
#include "mc6847_font.h"
#endif

static mc6847_t g_vdg;
static uint16_t g_panel[ATOM_SCREEN_H][ATOM_SCREEN_W];
static uint16_t g_full[ATOM_SCREEN_H][ATOM_SCREEN_W];
static uint8_t  g_vram[ATOM_VRAM_SIZE];
static uint8_t  g_shadow[ATOM_VRAM_SIZE];
static snappool_t g_pool;

static void render_full(uint16_t (*dst)[ATOM_SCREEN_W]) {
    for (unsigned y = 0; y < ATOM_SCREEN_H; y++)
        mc6847_render_row(&g_vdg, g_vram, y, dst[y]);
}

/* What display.c does per snapshot, minus the wire. Returns the number
 * of pixels it drew. */
static unsigned present_dirty(uint8_t mode) {
    uint16_t row[ATOM_SCREEN_W];
    unsigned pixels = 0;
    for (unsigned band = 0; band < ATOM_BAND_COUNT; band++) {
        uint16_t x0, x1;
        if (!mc6847_band_span(mode, g_vram, g_shadow, band, &x0, &x1)) continue;
        for (unsigned r = 0; r < ATOM_BAND_ROWS; r++) {
            unsigned y = band * ATOM_BAND_ROWS + r;
            mc6847_render_row(&g_vdg, g_vram, y, row);
            memcpy(&g_panel[y][x0], &row[x0], (size_t)(x1 - x0 + 1u) * 2u);
            pixels += x1 - x0 + 1u;
        }
    }
    memcpy(g_shadow, g_vram, sizeof(g_shadow));
    return pixels;
}

static unsigned rng_state = 12345;
static unsigned rng(void) {
    rng_state = rng_state * 1103515245u + 12345u;
    return (rng_state >> 16) & 0x7FFFu;
}

int main(void) {
    mc6847_init(&g_vdg);
#if PICO_ATOM_HAVE_FONT
    mc6847_set_font(&g_vdg, font_6847);
#endif

    /* ---- every mode: incremental presents match a full render -------- */
    for (unsigned m = 0; m < 9; m++) {
        for (unsigned css = 0; css < 2; css++) {
            uint8_t mode = (uint8_t)((m == 0 ? 0 : (VDG_AG | ((m - 1u) << VDG_GM_SHIFT)))
                                     | (css ? VDG_CSS : 0));
            const mc6847_mode_info_t *info = mc6847_mode_info(mode);
            mc6847_set_mode(&g_vdg, mode);

            for (unsigned i = 0; i < sizeof(g_vram); i++) g_vram[i] = (uint8_t)rng();
            memcpy(g_shadow, g_vram, sizeof(g_shadow));
            render_full(g_panel);

            for (unsigned step = 0; step < 50; step++) {
                /* A handful of scattered writes, then one present. */
                unsigned n = 1u + rng() % 6u;
                for (unsigned k = 0; k < n; k++)
                    g_vram[rng() % info->vram_bytes] ^= (uint8_t)(1u + rng() % 255u);
                present_dirty(mode);
                render_full(g_full);
                CHECK(memcmp(g_panel, g_full, sizeof(g_panel)) == 0,
                      "%s css=%u step %u: a dirty-band present left stale pixels",
                      info->name, css, step);
            }

            /* Nothing changed: nothing is drawn. */
            CHECK(present_dirty(mode) == 0,
                  "%s: an unchanged snapshot should present no pixels", info->name);
        }
    }

    /* ---- one byte changes one span, not the whole band --------------- */
    {
        uint8_t mode = 0;   /* alpha */
        mc6847_set_mode(&g_vdg, mode);
        memset(g_vram, 0x20, sizeof(g_vram));
        memcpy(g_shadow, g_vram, sizeof(g_shadow));
        g_vram[5] = 0x01;   /* cell (5, 0): 12 rows, band 0 and half of 1 */
        uint16_t x0 = 0, x1 = 0;
        CHECK(mc6847_band_span(mode, g_vram, g_shadow, 0, &x0, &x1) && x0 == 40 && x1 == 47,
              "one alpha cell should dirty columns 40..47 of band 0, got %u..%u", x0, x1);
        CHECK(mc6847_band_span(mode, g_vram, g_shadow, 1, &x0, &x1),
              "an alpha cell row straddles bands 0 and 1; band 1 must be marked too");
        CHECK(!mc6847_band_span(mode, g_vram, g_shadow, 2, &x0, &x1),
              "band 2 does not touch cell row 0");
    }

    /* ---- the row-source key: repeats only where rows are identical --- */
    {
        uint8_t cg1 = VDG_AG;   /* GM=000, y_scale 3 */
        CHECK(mc6847_row_source(cg1, 0) == mc6847_row_source(cg1, 2) &&
              mc6847_row_source(cg1, 2) != mc6847_row_source(cg1, 3),
              "CG1 rows 0..2 share a source row and row 3 does not");
        CHECK(mc6847_row_source(0, 0) != mc6847_row_source(0, 1),
              "alpha rows are distinct glyph rows and must not be reused");
    }

    /* ---- snapshot pool (§4.2) ---------------------------------------- */
    {
        snappool_t *p = &g_pool;
        snappool_init(p);

        CHECK(snappool_take(p) < 0, "nothing to take from an empty pool");

        int a = snappool_claim(p);
        CHECK(a >= 0, "claim from an empty pool should succeed");
        CHECK(snappool_take(p) < 0, "a filling buffer is invisible to core 1");
        snappool_publish(p, a);

        int r = snappool_take(p);
        CHECK(r == a, "core 1 should take the published buffer");

        /* Core 1 is a full field behind: core 0 publishes twice while it
         * renders. The first is superseded, and core 0 never runs dry. */
        int b = snappool_claim(p);
        snappool_publish(p, b);
        int c = snappool_claim(p);
        CHECK(c >= 0, "core 0 must never run out of buffers (§4.2's third buffer)");
        snappool_publish(p, c);
        CHECK(p->dropped == 1, "the older ready snapshot should be dropped, dropped=%u",
              (unsigned)p->dropped);
        int d = snappool_claim(p);
        CHECK(d == b, "the superseded buffer should be free again for core 0");

        snappool_release(p, r);
        CHECK(snappool_take(p) == c, "core 1 should get the newest snapshot, not the oldest");

        /* Long run, randomised interleaving: core 0 always gets a buffer
         * and never writes one core 1 holds. */
        snappool_init(p);
        int filling = snappool_claim(p), rendering = -1;
        for (unsigned step = 0; step < 10000; step++) {
            if (rng() & 1u) {
                snappool_publish(p, filling);
                filling = snappool_claim(p);
                CHECK(filling >= 0 && filling != rendering,
                      "step %u: core 0 claimed %d while core 1 renders %d",
                      step, filling, rendering);
            } else if (rendering < 0) {
                rendering = snappool_take(p);
            } else {
                snappool_release(p, rendering);
                rendering = -1;
            }
        }
    }

    TEST_DONE();
}
