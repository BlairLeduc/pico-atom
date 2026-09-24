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
#if PICO_ATOM_HAVE_FONT
#include "mc6847_font.h"
#endif
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

    /* ---- the port A mode nibble, off the circuit diagram (§16) -------
     *
     * bit 4 = A/G, bit 5 = GM0, bit 6 = GM1, bit 7 = GM2. Each bit is
     * exercised on its own so a transposition cannot hide behind a
     * symmetric test value. */
    {
        CHECK(mc6847_pack_mode(0x00, false) == mode_of(false, 0, false),
              "an empty nibble is alpha");
        CHECK(mc6847_pack_mode(0x10, false) == mode_of(true, 0, false),
              "bit 4 alone should be graphics with GM2:0 = 0 (CG1)");
        CHECK(mc6847_pack_mode(0xF0, false) == mode_of(true, 7, false),
              "a full nibble should be RG6");

        /* One GM bit at a time, with A/G set. */
        CHECK(mc6847_pack_mode(0x30, false) == mode_of(true, 1, false),
              "bit 5 is GM0, so 0x30 should be RG1");
        CHECK(mc6847_pack_mode(0x50, false) == mode_of(true, 2, false),
              "bit 6 is GM1, so 0x50 should be CG2");
        CHECK(mc6847_pack_mode(0x90, false) == mode_of(true, 4, false),
              "bit 7 is GM2, so 0x90 should be CG3");

        /* Bit 4 clear is alpha whatever the GM bits say. */
        for (unsigned n = 0; n < 8u; n++) {
            uint8_t nibble = (uint8_t)((n << 5) & 0xE0u);
            CHECK(mc6847_mode_info(mc6847_pack_mode(nibble, false))->kind == VDG_ALPHA,
                  "nibble 0x%02X has bit 4 clear and must be alpha", nibble);
        }

        /* Every GM value is reachable and distinct. */
        bool seen[8] = { false };
        for (unsigned n = 0; n < 8u; n++) {
            uint8_t nibble = (uint8_t)(0x10u | ((n << 5) & 0xE0u));
            unsigned gm = (mc6847_pack_mode(nibble, false) & VDG_GM_MASK) >> VDG_GM_SHIFT;
            CHECK(!seen[gm], "GM value %u reached twice", gm);
            seen[gm] = true;
        }
        for (unsigned gm = 0; gm < 8u; gm++)
            CHECK(seen[gm], "GM value %u is unreachable from port A", gm);

        /* CSS rides along from port C. */
        CHECK((mc6847_pack_mode(0x10, true) & VDG_CSS) != 0,
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

    /* ---- SG6: six elements, colour from bits 7 and 6 ---------------- */
    {
        mc6847_init(&g_vdg);
        mc6847_set_mode(&g_vdg, mode_of(false, 0, false));  /* alpha/SG6 */
        memset(g_vram, 0, sizeof(g_vram));

        /* Bit 6 -> semigraphics; bit 7 clear -> C1:C0 = 01, yellow;
         * top-left element only. */
        g_vram[0] = (uint8_t)(VDG_BYTE_SG6 | 0x20u);

        mc6847_render_row(&g_vdg, g_vram, 0, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_YELLOW], "SG6 top-left should be lit");
        CHECK(g_row[3] == mc6847_palette[VDG_YELLOW], "SG6 element spans four pixels");
        CHECK(g_row[4] == mc6847_palette[VDG_BLACK], "SG6 top-right should be dark");
        mc6847_render_row(&g_vdg, g_vram, 3, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_YELLOW], "SG6 element spans four rows");
        mc6847_render_row(&g_vdg, g_vram, 4, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_BLACK], "SG6 middle-left should be dark");

        /* All six elements are reachable and independent, in the order
         * BASIC's PLOT sets them (test_boot pins that against the ROM). */
        const unsigned el_x[6] = { 4, 0, 4, 0, 4, 0 };      /* bit 0..5 */
        const unsigned el_y[6] = { 8, 8, 4, 4, 0, 0 };
        for (unsigned e = 0; e < 6u; e++) {
            g_vram[0] = (uint8_t)(VDG_BYTE_SG6 | (1u << e));
            for (unsigned y = 0; y < 12u; y += 4u) {
                mc6847_render_row(&g_vdg, g_vram, y, g_row);
                for (unsigned x = 0; x < 8u; x += 4u) {
                    bool want = (x == el_x[e] && y == el_y[e]);
                    CHECK((g_row[x] == mc6847_palette[VDG_YELLOW]) == want,
                          "SG6 bit %u at (%u,%u) should be %s", e, x, y,
                          want ? "lit" : "dark");
                }
            }
        }

        /* Bit 7 is C1: red, and cyan/orange under CSS. It is not an
         * inverse in a graphics cell. */
        g_vram[0] = (uint8_t)(VDG_BYTE_INV | VDG_BYTE_SG6 | 0x20u);
        mc6847_render_row(&g_vdg, g_vram, 0, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_RED], "bit 7 set: red");
        CHECK(g_row[4] == mc6847_palette[VDG_BLACK], "bit 7 does not invert SG6");
        mc6847_set_mode(&g_vdg, mode_of(false, 0, true));
        mc6847_render_row(&g_vdg, g_vram, 0, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_ORANGE], "bit 7 set, CSS: orange");
        g_vram[0] = (uint8_t)(VDG_BYTE_SG6 | 0x20u);
        mc6847_render_row(&g_vdg, g_vram, 0, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_CYAN], "bit 7 clear, CSS: cyan");
    }

    /* ---- alpha: glyph lookup, inverse video, and the missing font ---- */
    {
        /* Flat, like a ROM dump: glyph g row r at font[g * 12 + r], with
         * the 5-wide glyph in bits 5..1. 0x2A is #.#.# in those bits —
         * a real row from the ROM — and is drawn as it stands, bit 7
         * leftmost, so it lands in columns 2..6. A value with bit 0 set
         * would be outside the 5-wide field and would light a spacing
         * column. */
        static uint8_t font[MC6847_FONT_BYTES];
        memset(font, 0, sizeof(font));
        font[5 * MC6847_FONT_ROWS + 3] = 0x2A;

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
            bool lit = (0x2Au >> (7u - x)) & 1u;
            CHECK(g_row[x] == mc6847_palette[lit ? VDG_GREEN : VDG_BLACK],
                  "alpha glyph pixel %u wrong", x);
        }
        /* The glyph is 5 wide in an 8-wide cell, two columns in from the
         * left: columns 0, 1 and 7 are spacing, and always background.
         * This is what put every character against the left edge when
         * the renderer shifted the glyph instead. */
        CHECK(g_row[MC6847_FONT_GLYPH_X] == mc6847_palette[VDG_GREEN],
              "the glyph's first column is cell column %u", MC6847_FONT_GLYPH_X);
        for (unsigned x = 0; x < 8u; x++) {
            bool spacing = x < MC6847_FONT_GLYPH_X ||
                           x >= MC6847_FONT_GLYPH_X + MC6847_FONT_GLYPH_W;
            if (spacing)
                CHECK(g_row[x] == mc6847_palette[VDG_BLACK],
                      "cell column %u is spacing and should be background", x);
        }

        /* A row the glyph does not set is background. */
        mc6847_render_row(&g_vdg, g_vram, 4, g_row);
        CHECK(g_row[0] == mc6847_palette[VDG_BLACK], "an empty glyph row is background");

        /* Bit 7 inverts; it is how the MOS draws its cursor. */
        g_vram[0] = (uint8_t)(5u | VDG_BYTE_INV);
        mc6847_render_row(&g_vdg, g_vram, 3, g_row);
        /* Row 0x2A is .#.#.#.. across the cell: ink at 2, gap at 3. */
        CHECK(g_row[2] == mc6847_palette[VDG_BLACK], "inverse video should swap fg and bg");
        CHECK(g_row[3] == mc6847_palette[VDG_GREEN], "inverse video should swap fg and bg");
        CHECK(g_row[0] == mc6847_palette[VDG_GREEN], "inverse fills the spacing columns too");

        /* Glyph index to ASCII is the MC6847's own order, not plain
         * ASCII: 0..31 are $40..$5F and 32..63 are $20..$3F. */
        CHECK(mc6847_glyph_ascii(0)  == 0x40u, "glyph 0 is '@'");
        CHECK(mc6847_glyph_ascii(1)  == 0x41u, "glyph 1 is 'A'");
        CHECK(mc6847_glyph_ascii(26) == 0x5Au, "glyph 26 is 'Z'");
        CHECK(mc6847_glyph_ascii(32) == 0x20u, "glyph 32 is space");
        CHECK(mc6847_glyph_ascii(63) == 0x3Fu, "glyph 63 is '?'");
        {
            bool seen[64] = { false };
            for (unsigned g = 0; g < 64u; g++) {
                unsigned a = mc6847_glyph_ascii((uint8_t)g);
                CHECK(a >= 0x20u && a < 0x60u, "glyph %u maps outside $20-$5F", g);
                CHECK(!seen[a - 0x20u], "ASCII 0x%02X reached twice", a);
                seen[a - 0x20u] = true;
            }
            for (unsigned i = 0; i < 64u; i++)
                CHECK(seen[i], "ASCII 0x%02X is unreachable", i + 0x20u);
        }

        /* CSS gives orange on black rather than green on black. */
        g_vram[0] = 5;
        mc6847_set_mode(&g_vdg, mode_of(false, 0, true));
        mc6847_render_row(&g_vdg, g_vram, 3, g_row);
        CHECK(g_row[MC6847_FONT_GLYPH_X] == mc6847_palette[VDG_ORANGE],
              "alpha with CSS should be orange");
    }

#if PICO_ATOM_HAVE_FONT
    /* ---- a zeroed VRAM page shows '@', and that is correct ----------
     *
     * Glyph 0 is '@', not the space, so an uncleared screen is a screen
     * of '@'. The MOS writes spaces when it wants a blank screen. An
     * emulator that substituted blanks here would hide a ROM that never
     * cleared the screen, so this is pinned down rather than left to
     * look like an oversight worth correcting. */
    {
        mc6847_init(&g_vdg);
        mc6847_set_font(&g_vdg, font_6847);
        mc6847_set_mode(&g_vdg, mode_of(false, 0, false));
        memset(g_vram, 0, sizeof(g_vram));

        unsigned ink = 0;
        for (unsigned y = 0; y < ATOM_SCREEN_H; y++) {
            mc6847_render_row(&g_vdg, g_vram, y, g_row);
            for (unsigned x = 0; x < ATOM_SCREEN_W; x++)
                if (g_row[x] != mc6847_palette[VDG_BLACK]) ink++;
        }
        CHECK(ink > 0, "a zeroed VRAM page should render '@', not blanks");

        /* And the space glyph really does give a blank page, so the
         * difference is the VRAM contents and not the renderer. */
        memset(g_vram, MC6847_GLYPH_SPACE, sizeof(g_vram));
        unsigned blank_ink = 0;
        for (unsigned y = 0; y < ATOM_SCREEN_H; y++) {
            mc6847_render_row(&g_vdg, g_vram, y, g_row);
            for (unsigned x = 0; x < ATOM_SCREEN_W; x++)
                if (g_row[x] != mc6847_palette[VDG_BLACK]) blank_ink++;
        }
        CHECK(blank_ink == 0, "a page of the space glyph should have no ink");
    }

    /* ---- the supplied ROM honours the layout the renderer assumes ---
     *
     * A ROM laid out differently — bit 7 leftmost, say — would render as
     * plausible-looking but wrong glyphs rather than failing loudly, so
     * the contract is checked against the data rather than assumed. */
    {
        unsigned used = 0;
        for (unsigned i = 0; i < MC6847_FONT_BYTES; i++) used |= font_6847[i];
        CHECK((used & 0xC1u) == 0,
              "the ROM uses bits outside 5..1 (union 0x%02X); the 5-wide "
              "glyph field is not where the renderer expects it", used);

        /* Rows 0-2 and 10-11 of every cell are vertical spacing. */
        for (unsigned g = 0; g < MC6847_FONT_GLYPHS; g++) {
            for (unsigned r = 0; r < 3u; r++)
                CHECK(font_6847[g * MC6847_FONT_ROWS + r] == 0,
                      "glyph %u row %u should be blank spacing", g, r);
            for (unsigned r = 10u; r < MC6847_FONT_ROWS; r++)
                CHECK(font_6847[g * MC6847_FONT_ROWS + r] == 0,
                      "glyph %u row %u should be blank spacing", g, r);
        }

        /* Glyph 32 is the space and must be entirely blank; the letters
         * either side of it must not be. */
        for (unsigned r = 0; r < MC6847_FONT_ROWS; r++)
            CHECK(font_6847[32u * MC6847_FONT_ROWS + r] == 0,
                  "glyph 32 is the space and should be blank");
        unsigned ink_a = 0, ink_z = 0;
        for (unsigned r = 0; r < MC6847_FONT_ROWS; r++) {
            ink_a |= font_6847[1u * MC6847_FONT_ROWS + r];
            ink_z |= font_6847[26u * MC6847_FONT_ROWS + r];
        }
        CHECK(ink_a != 0, "glyph 1 is 'A' and should have ink");
        CHECK(ink_z != 0, "glyph 26 is 'Z' and should have ink");
    }
#endif

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

    /* ---- monochrome: luminance alone (§8.7) --------------------------- */
    {
        /* CG6, CSS 0, one byte of each colour: green yellow blue red. */
        uint8_t mode = mode_of(true, 6, false);
        memset(g_vram, 0, sizeof g_vram);
        g_vram[0] = 0x1B;
        mc6847_init(&g_vdg);
        mc6847_set_mode(&g_vdg, mode);
        mc6847_render_row(&g_vdg, g_vram, 0, g_row);
        uint16_t colour[4] = { g_row[0], g_row[2], g_row[4], g_row[6] };
        CHECK(colour[2] != colour[3], "in colour, blue and red differ");

        mc6847_set_mono(&g_vdg, true);
        mc6847_render_row(&g_vdg, g_vram, 0, g_row);
        uint16_t black = mc6847_palette_mono[VDG_BLACK];
        CHECK(g_row[4] == g_row[6], "in mono, blue and red are one level");
        CHECK(g_row[2] == 0xFFFFu, "yellow, at 0.42 V, is white: %04X", g_row[2]);
        CHECK(g_row[4] != black && g_row[4] < g_row[0], "blue, at 0.65 V, is dark grey: %04X",
              g_row[4]);
        CHECK(g_row[0] != black && g_row[0] < g_row[2], "green, at 0.54 V, is the grey between");
        for (unsigned i = 0; i < VDG_COLOUR_COUNT; i++) {
            uint16_t c = mc6847_palette_mono[i];
            CHECK((c >> 11) == (c & 0x1Fu) && ((c >> 5) & 0x3Fu) >> 1 == (c & 0x1Fu),
                  "mono colour %u is a grey: %04X", i, c);
        }

        /* Text too, and back to colour gives back the colour row. */
        mc6847_set_mode(&g_vdg, mode_of(false, 0, false));
        g_vram[0] = 0xA0;                            /* an inverse space: all ink */
        mc6847_render_row(&g_vdg, g_vram, 1, g_row); /* blank with or without a font */
        CHECK(g_row[2] == mc6847_palette_mono[VDG_GREEN], "alpha ink in mono: %04X", g_row[2]);
        mc6847_set_mono(&g_vdg, false);
        mc6847_set_mode(&g_vdg, mode);
        g_vram[0] = 0x1B;
        mc6847_render_row(&g_vdg, g_vram, 0, g_row);
        CHECK(g_row[0] == colour[0] && g_row[6] == colour[3], "colour again");
    }

    /* ---- the border (§8.7) -------------------------------------------- */
    {
        CHECK(mc6847_border(mode_of(false, 0, false)) == VDG_BLACK &&
              mc6847_border(mode_of(false, 0, true)) == VDG_BLACK, "alpha: black");
        for (unsigned gm = 0; gm < 8u; gm++) {
            CHECK(mc6847_border(mode_of(true, gm, false)) == VDG_GREEN, "GM %u CSS 0: green", gm);
            CHECK(mc6847_border(mode_of(true, gm, true)) == VDG_BUFF, "GM %u CSS 1: buff", gm);
        }
    }

    TEST_DONE();
}
