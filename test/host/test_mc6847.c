/* test_mc6847.c — MC6847 mode decode, expansion LUT and row generation
 * (design.md §8.3, §15.1).
 *
 * The mode table of §2.4 and the stretch factors of §8.3 are two
 * descriptions of the same nine modes, and they have to agree. The
 * invariants asserted first are what tie them together; they would have
 * caught any single transposed number in either table.
 */

#include <string.h>

#include "config.h"
#include "mc6847.h"
#include "test_util.h"

static mc6847_t g_vdg;
static uint8_t  g_vram[ATOM_VRAM_SIZE];
static uint16_t g_row[ATOM_SCREEN_W];

static uint8_t mode_of(bool ag, unsigned gm, bool css) {
    return (uint8_t)((ag ? VDG_AG : 0u) |
                     ((gm << VDG_GM_SHIFT) & VDG_GM_MASK) |
                     (css ? VDG_CSS : 0u));
}

int main(void) {
    /* ---- §2.4 and §8.3 have to describe the same nine modes --------- */
    for (unsigned i = 0; i < 9u; i++) {
        bool alpha = (i == 0);
        uint8_t mode = alpha ? mode_of(false, 0, false)
                             : mode_of(true, i - 1u, false);
        const mc6847_mode_info_t *info = mc6847_mode_info(mode);

        CHECK(info->bytes_per_row * info->px_per_byte == ATOM_SCREEN_W,
              "%s: %u bytes x %u px should span 256, got %u",
              info->name, info->bytes_per_row, info->px_per_byte,
              info->bytes_per_row * info->px_per_byte);
        CHECK(info->rows * info->y_scale == ATOM_SCREEN_H,
              "%s: %u rows x %u should span 192, got %u",
              info->name, info->rows, info->y_scale, info->rows * info->y_scale);
        CHECK(info->bytes_per_row * info->rows == info->vram_bytes,
              "%s: %u x %u should be %u bytes of VRAM",
              info->name, info->bytes_per_row, info->rows, info->vram_bytes);
        CHECK(info->vram_bytes <= ATOM_VRAM_SIZE,
              "%s wants %u bytes, more than the 6 KiB that exists",
              info->name, info->vram_bytes);
        CHECK(info->px_per_byte <= ATOM_LUT_ENTRY_MAX / 2u,
              "%s needs a LUT entry larger than config.h allows", info->name);
    }

    /* Every graphics mode byte reaches a distinct mode; alpha ignores GM. */
    {
        for (unsigned gm = 0; gm < 8u; gm++) {
            CHECK(mc6847_mode_info(mode_of(false, gm, false))->kind == VDG_ALPHA,
                  "A/G low should select alpha whatever GM2:0 says");
        }
        const char *want[8] = { "CG1", "RG1", "CG2", "RG2", "CG3", "RG3", "CG6", "RG6" };
        for (unsigned gm = 0; gm < 8u; gm++) {
            const mc6847_mode_info_t *info = mc6847_mode_info(mode_of(true, gm, false));
            CHECK(strcmp(info->name, want[gm]) == 0,
                  "GM2:0 = %u should be %s, got %s", gm, want[gm], info->name);
        }
    }

    /* ---- the port A bit order, both readings (§16) ------------------- */
    {
        /* §2.4: bit7 = A/G, bit6 = GM2, bit5 = GM1, bit4 = GM0. */
        CHECK(mc6847_pack_mode(0x80, false, VDG_BITS_AG_HIGH) == mode_of(true, 0, false),
              "AG_HIGH: 0x80 should be graphics with GM2:0 = 0");
        CHECK(mc6847_pack_mode(0xF0, false, VDG_BITS_AG_HIGH) == mode_of(true, 7, false),
              "AG_HIGH: 0xF0 should be RG6");
        CHECK(mc6847_pack_mode(0x40, false, VDG_BITS_AG_HIGH) == mode_of(false, 4, false),
              "AG_HIGH: bit 7 clear is alpha whatever else is set");

        /* §2.3: bit4 = A/G, bit5 = GM0, bit6 = GM1, bit7 = GM2. */
        CHECK(mc6847_pack_mode(0x10, false, VDG_BITS_AG_LOW) == mode_of(true, 0, false),
              "AG_LOW: 0x10 should be graphics with GM2:0 = 0");
        CHECK(mc6847_pack_mode(0xF0, false, VDG_BITS_AG_LOW) == mode_of(true, 7, false),
              "AG_LOW: 0xF0 should be RG6");
        CHECK(mc6847_pack_mode(0x20, false, VDG_BITS_AG_LOW) == mode_of(false, 1, false),
              "AG_LOW: bit 4 clear is alpha");

        /* The two readings genuinely differ, which is why this is
         * configuration and not a constant. */
        CHECK(mc6847_pack_mode(0x80, false, VDG_BITS_AG_HIGH) !=
              mc6847_pack_mode(0x80, false, VDG_BITS_AG_LOW),
              "the two bit orders should disagree about 0x80");

        /* CSS rides along from port C either way. */
        CHECK((mc6847_pack_mode(0x80, true, VDG_BITS_AG_HIGH) & VDG_CSS) != 0,
              "CSS should reach the mode byte");
    }

    /* ---- RG6 is 1:1, one bit per pixel, MSB first -------------------- */
    {
        mc6847_init(&g_vdg);
        mc6847_set_mode(&g_vdg, mode_of(true, 7, false));   /* RG6 */
        memset(g_vram, 0, sizeof(g_vram));
        g_vram[0] = 0x81;                                   /* 1000 0001 */

        mc6847_render_row(&g_vdg, g_vram, 0, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_GREEN], "RG6 bit 7 should be the leftmost pixel");
        for (unsigned x = 1; x < 7u; x++)
            CHECK(g_row[x] == mc6847_palette[VDG_BLACK], "RG6 pixel %u should be black", x);
        CHECK(g_row[7] == mc6847_palette[VDG_GREEN], "RG6 bit 0 should be the eighth pixel");

        /* CSS swaps green for buff in the RG modes. */
        mc6847_set_mode(&g_vdg, mode_of(true, 7, true));
        mc6847_render_row(&g_vdg, g_vram, 0, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_BUFF], "RG6 with CSS should use buff");
    }

    /* ---- CG1 stretches 4 px per byte by four horizontally ------------ */
    {
        mc6847_init(&g_vdg);
        mc6847_set_mode(&g_vdg, mode_of(true, 0, false));   /* CG1 */
        memset(g_vram, 0, sizeof(g_vram));
        g_vram[0] = 0x1B;      /* 00 01 10 11 -> green, yellow, blue, red */

        mc6847_render_row(&g_vdg, g_vram, 0, g_row);
        const vdg_colour_t want[4] = { VDG_GREEN, VDG_YELLOW, VDG_BLUE, VDG_RED };
        for (unsigned p = 0; p < 4u; p++)
            for (unsigned s = 0; s < 4u; s++)
                CHECK(g_row[p * 4u + s] == mc6847_palette[want[p]],
                      "CG1 pixel %u repeat %u should be colour %u", p, s, want[p]);

        /* CSS selects the second four-colour set. */
        mc6847_set_mode(&g_vdg, mode_of(true, 0, true));
        mc6847_render_row(&g_vdg, g_vram, 0, g_row);
        const vdg_colour_t want_css[4] = { VDG_BUFF, VDG_CYAN, VDG_MAGENTA, VDG_ORANGE };
        for (unsigned p = 0; p < 4u; p++)
            CHECK(g_row[p * 4u] == mc6847_palette[want_css[p]],
                  "CG1 with CSS pixel %u wrong", p);
    }

    /* ---- vertical stretch repeats a source row, it does not skip ----- */
    {
        mc6847_init(&g_vdg);
        mc6847_set_mode(&g_vdg, mode_of(true, 0, false));   /* CG1, y_scale 3 */
        memset(g_vram, 0, sizeof(g_vram));
        g_vram[0]  = 0xFF;    /* source row 0 */
        g_vram[16] = 0x00;    /* source row 1 */

        for (unsigned y = 0; y < 3u; y++) {
            mc6847_render_row(&g_vdg, g_vram, y, g_row);
            CHECK(g_row[0] == mc6847_palette[VDG_RED],
                  "CG1 display row %u should still be source row 0", y);
        }
        mc6847_render_row(&g_vdg, g_vram, 3, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_GREEN],
              "CG1 display row 3 should be source row 1");
    }

    /* ---- SG4: four quadrants and a colour in the top nibble ---------- */
    {
        mc6847_init(&g_vdg);
        mc6847_set_mode(&g_vdg, mode_of(false, 0, false));  /* alpha/SG4 */
        memset(g_vram, 0, sizeof(g_vram));

        /* bit 7 set -> semigraphics; colour 3 (red); top-left quadrant. */
        g_vram[0] = (uint8_t)(0x80u | (3u << 4) | 0x08u);

        mc6847_render_row(&g_vdg, g_vram, 0, g_row);       /* upper half */
        CHECK(g_row[0] == mc6847_palette[VDG_RED], "SG4 top-left should be lit");
        CHECK(g_row[3] == mc6847_palette[VDG_RED], "SG4 top-left spans four pixels");
        CHECK(g_row[4] == mc6847_palette[VDG_BLACK], "SG4 top-right should be dark");

        mc6847_render_row(&g_vdg, g_vram, 6, g_row);       /* lower half */
        CHECK(g_row[0] == mc6847_palette[VDG_BLACK], "SG4 bottom-left should be dark");

        /* All four quadrants are reachable and independent. */
        const unsigned quad_bit[4] = { 3, 2, 1, 0 };
        const unsigned quad_y[4]   = { 0, 0, 6, 6 };
        const unsigned quad_x[4]   = { 0, 4, 0, 4 };
        for (unsigned q = 0; q < 4u; q++) {
            g_vram[0] = (uint8_t)(0x80u | (1u << 4) | (1u << quad_bit[q]));
            mc6847_render_row(&g_vdg, g_vram, quad_y[q], g_row);
            CHECK(g_row[quad_x[q]] == mc6847_palette[VDG_YELLOW],
                  "SG4 quadrant %u should light at (%u,%u)", q, quad_x[q], quad_y[q]);
        }
    }

    /* ---- alpha: glyph lookup, inverse video, and the missing font ---- */
    {
        static uint8_t font[MC6847_FONT_GLYPHS][MC6847_FONT_ROWS];
        memset(font, 0, sizeof(font));
        font[5][3] = 0xA5;          /* a recognisable glyph row */

        mc6847_init(&g_vdg);
        mc6847_set_mode(&g_vdg, mode_of(false, 0, false));
        memset(g_vram, 0, sizeof(g_vram));

        /* With no character ROM the placeholder is drawn, and it must not
         * be mistaken for a real glyph (§16). */
        CHECK(!mc6847_has_font(&g_vdg), "a fresh VDG has no character ROM");
        g_vram[0] = 5;
        mc6847_render_row(&g_vdg, g_vram, 3, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_GREEN],
              "the placeholder should draw its left edge");

        mc6847_set_font(&g_vdg, font);
        CHECK(mc6847_has_font(&g_vdg), "the character ROM should be taken");

        mc6847_render_row(&g_vdg, g_vram, 3, g_row);
        for (unsigned x = 0; x < 8u; x++) {
            bool lit = ((0xA5u >> (7u - x)) & 1u) != 0;
            CHECK(g_row[x] == mc6847_palette[lit ? VDG_GREEN : VDG_BLACK],
                  "alpha glyph pixel %u wrong", x);
        }

        /* A row the glyph does not set is background. */
        mc6847_render_row(&g_vdg, g_vram, 4, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_BLACK], "an empty glyph row is background");

        /* Bit 6 inverts. */
        g_vram[0] = (uint8_t)(5u | 0x40u);
        mc6847_render_row(&g_vdg, g_vram, 3, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_BLACK], "inverse video should swap fg and bg");
        CHECK(g_row[1] == mc6847_palette[VDG_GREEN], "inverse video should swap fg and bg");

        /* CSS gives orange on black rather than green on black. */
        g_vram[0] = 5;
        mc6847_set_mode(&g_vdg, mode_of(false, 0, true));
        mc6847_render_row(&g_vdg, g_vram, 3, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_ORANGE], "alpha with CSS should be orange");
    }

    /* ---- a full frame in every mode writes exactly 256 px per row ---- */
    {
        for (unsigned i = 0; i < 9u; i++) {
            for (unsigned css = 0; css < 2u; css++) {
                bool alpha = (i == 0);
                uint8_t mode = alpha ? mode_of(false, 0, css != 0)
                                     : mode_of(true, i - 1u, css != 0);
                mc6847_init(&g_vdg);
                mc6847_set_mode(&g_vdg, mode);
                for (unsigned b = 0; b < sizeof(g_vram); b++)
                    g_vram[b] = (uint8_t)(b * 7u + i);

                for (unsigned y = 0; y < ATOM_SCREEN_H; y++) {
                    const uint16_t guard = 0xDEADu;
                    g_row[ATOM_SCREEN_W - 1u] = guard;
                    mc6847_render_row(&g_vdg, g_vram, y, g_row);
                    CHECK(g_row[ATOM_SCREEN_W - 1u] != guard,
                          "%s row %u did not fill the last pixel",
                          mc6847_mode_info(mode)->name, y);
                }
            }
        }
    }

    TEST_DONE();
}
