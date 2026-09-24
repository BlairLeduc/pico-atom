/* display.c — snapshot diff and band present (design.md §8.2-8.4). */

#include "display.h"

#include <string.h>

#include "pico/stdlib.h"

#include "config.h"
#include "lcd.h"
#include "mc6847.h"

#define RGB565(r, g, b) \
    (uint16_t)((((r) & 0xF8u) << 8) | (((g) & 0xFCu) << 3) | ((b) >> 3))

/* The renderer lives here, on core 1, so its LUT is rebuilt by the only
 * core that reads it (§8.4). */
static mc6847_t s_vdg;
static uint8_t  s_shadow[ATOM_VRAM_SIZE];
static uint8_t  s_presented_mode;
static bool     s_valid;

/* §8.7's settings, and the colour the border was last filled with:
 * black to begin with, since lcd_init clears the panel. Nothing else
 * draws outside the rectangle, so an invalidate leaves it be, and with
 * the border off it is never filled at all. */
static bool     s_border;
static uint16_t s_border_rgb = 0x0000u;

/* DMA ping-pong (§4.6); sized for the full panel width so the status
 * band can use them too. */
static uint16_t s_line[ATOM_LINEBUF_COUNT][ATOM_LINEBUF_PIXELS];

void display_init(const uint8_t *font) {
    mc6847_init(&s_vdg);
    if (font) mc6847_set_font(&s_vdg, font);
    s_valid = false;
}

void display_invalidate(void) {
    s_valid = false;
}

void display_set_look(bool mono, bool border) {
    mc6847_set_mono(&s_vdg, mono);
    s_border = border;
    display_invalidate();
}

/* The panel around the Atom's rectangle, in four fills: 53,248 pixels,
 * about as many as the rectangle itself, so it goes only when its
 * colour changes, a mode change between alpha and graphics or of CSS in
 * graphics (§8.7). */
static bool fill_border(uint8_t mode) {
    uint16_t rgb = s_border ? s_vdg.pal[mc6847_border(mode)] : 0x0000u;
    if (rgb == s_border_rgb) return false;
    const unsigned x1 = ATOM_SCREEN_X + ATOM_SCREEN_W, y1 = ATOM_SCREEN_Y + ATOM_SCREEN_H;
    lcd_fill(0, 0, ATOM_PANEL_W, ATOM_SCREEN_Y, rgb);
    lcd_fill(0, y1, ATOM_PANEL_W, ATOM_PANEL_H - y1, rgb);
    lcd_fill(0, ATOM_SCREEN_Y, ATOM_SCREEN_X, ATOM_SCREEN_H, rgb);
    lcd_fill(x1, ATOM_SCREEN_Y, ATOM_PANEL_W - x1, ATOM_SCREEN_H, rgb);
    s_border_rgb = rgb;
    return true;
}

/* Send display rows [y0, y0+h) of columns [x0..x1] as one window.
 * Rows that share a source row are sent from the same line buffer
 * without being regenerated: vertical stretch is free (§8.3). */
static void send_rows(const uint8_t *vram, uint8_t mode,
                      unsigned y0, unsigned h, unsigned x0, unsigned x1) {
    unsigned w = x1 - x0 + 1u;
    unsigned cur = 0;
    unsigned last_src = ~0u;

    lcd_blit_begin(ATOM_SCREEN_X + x0, ATOM_SCREEN_Y + y0, w, h);
    for (unsigned y = y0; y < y0 + h; y++) {
        unsigned src = mc6847_row_source(mode, y);
        if (src != last_src) {
            /* The buffer not on the wire: lcd_blit_row waits out the
             * previous DMA before starting, so the other one is free. */
            cur ^= 1u;
            mc6847_render_row(&s_vdg, vram, y, s_line[cur]);
            last_src = src;
        }
        lcd_blit_row(&s_line[cur][x0], w);
    }
    lcd_blit_end();
}

void display_present(const uint8_t *vram, uint8_t mode, display_stats_t *st) {
    uint32_t t0 = time_us_32();
    display_stats_t s = { 0 };

    if (!s_valid || mode != s_presented_mode) {
        /* §8.4 step 1: a correctness requirement, not an optimisation. */
        mc6847_set_mode(&s_vdg, mode);
        s.border = fill_border(mode);
        send_rows(vram, mode, 0, ATOM_SCREEN_H, 0, ATOM_SCREEN_W - 1u);
        s.full = true;
        s.bands = ATOM_BAND_COUNT;
        s.pixels = ATOM_SCREEN_W * ATOM_SCREEN_H +
                   (s.border ? ATOM_PANEL_W * ATOM_PANEL_H - ATOM_SCREEN_W * ATOM_SCREEN_H : 0u);
    } else {
        for (unsigned band = 0; band < ATOM_BAND_COUNT; band++) {
            uint16_t x0, x1;
            if (!mc6847_band_span(mode, vram, s_shadow, band, &x0, &x1)) continue;
            send_rows(vram, mode, band * ATOM_BAND_ROWS, ATOM_BAND_ROWS, x0, x1);
            s.bands++;
            s.pixels += (uint32_t)(x1 - x0 + 1u) * ATOM_BAND_ROWS;
        }
    }

    /* The mode byte is part of the shadow, not merely of the snapshot. */
    memcpy(s_shadow, vram, sizeof(s_shadow));
    s_presented_mode = mode;
    s_valid = true;

    s.us = time_us_32() - t0;
    if (st) *st = s;
}

void display_test_pattern(void) {
    const unsigned x = ATOM_SCREEN_X, y = ATOM_SCREEN_Y;
    const unsigned w = ATOM_SCREEN_W, h = ATOM_SCREEN_H;
    const uint16_t white = RGB565(0xFF, 0xFF, 0xFF);

    lcd_fill(x, y, w, h, 0x0000);

    lcd_fill(x, y, w, 1, white);               /* top    */
    lcd_fill(x, y + h - 1u, w, 1, white);      /* bottom */
    lcd_fill(x, y, 1, h, white);               /* left   */
    lcd_fill(x + w - 1u, y, 1, h, white);      /* right  */

    lcd_fill(x + 2u, y + 2u, 16, 16, RGB565(0xFF, 0x00, 0x00));
    lcd_fill(x + w - 18u, y + 2u, 16, 16, RGB565(0x00, 0xFF, 0x00));
    lcd_fill(x + 2u, y + h - 18u, 16, 16, RGB565(0x00, 0x00, 0xFF));
    lcd_fill(x + w - 18u, y + h - 18u, 16, 16, RGB565(0xFF, 0xFF, 0x00));

    display_invalidate();
}
