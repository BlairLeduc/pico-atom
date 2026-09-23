/* config.h — every fixed capacity in the emulator, in one place.
 *
 * design.md §5: SRAM is the scarce resource and the budget is only a
 * link-time fact if the capacities live together. Nothing in src/core/
 * allocates; every buffer is sized from a constant here.
 *
 * Guest addresses are written #XXXX in comments and 0x in code.
 */
#ifndef PICO_ATOM_CONFIG_H
#define PICO_ATOM_CONFIG_H

/* ---- Guest address space (design.md §2.2, §7.1) ---------------------- */

#define ATOM_ADDR_SPACE     65536u  /* flat uint8_t ram[], direct index    */
#define ATOM_PAGE_SIZE        256u
#define ATOM_PAGE_COUNT     (ATOM_ADDR_SPACE / ATOM_PAGE_SIZE)

#define ATOM_VRAM_BASE     0x8000u  /* #8000                              */
#define ATOM_VRAM_SIZE       6144u  /* #8000-#97FF                        */

#define ATOM_ROM_BASE      0xA000u  /* utility socket onwards             */

/* ---- Video (design.md §8) -------------------------------------------- */

#define ATOM_SCREEN_W         256u
#define ATOM_SCREEN_H         192u

/* Native 256x192 at panel offset (32,64) — hardware-notes.md §4.10. */
#define ATOM_PANEL_W          320u
#define ATOM_PANEL_H          320u
#define ATOM_SCREEN_X          32u
#define ATOM_SCREEN_Y          64u

#define ATOM_SNAPSHOT_COUNT     3u  /* §4.2: three, and the third is the point */
#define ATOM_BAND_ROWS          8u  /* 24 bands over 192 rows (§8.4)      */
#define ATOM_BAND_COUNT     (ATOM_SCREEN_H / ATOM_BAND_ROWS)

/* One VRAM byte expands to at most 16 RGB565 pixels = 32 bytes (§8.3). */
#define ATOM_LUT_ENTRY_MAX     32u
#define ATOM_LUT_SIZE       (256u * ATOM_LUT_ENTRY_MAX)

#define ATOM_LINEBUF_COUNT      2u  /* DMA ping-pong (§4.6)               */
#define ATOM_LINEBUF_PIXELS  ATOM_PANEL_W

#define ATOM_PALETTE_SIZE      16u  /* 9 VDG colours + UI (§2.4)          */

#define ATOM_FONT_GLYPHS       64u  /* MC6847 character ROM               */
#define ATOM_FONT_ROWS         12u  /* 5x7 in an 8x12 cell                */

/* ---- Timing (design.md §12.1) ---------------------------------------- */

#define ATOM_CPU_HZ       1000000u  /* stock 1 MHz; turbo is a multiplier */

/* §16 lists the field rate as medium confidence (50 or 60 Hz on a UK
 * Atom). §18 says make it configuration, not a constant, so the default
 * lives here and the machine carries it in atom_t. */
#define ATOM_FIELD_HZ_DEFAULT  60u

/* FS (port C bit 7) is low for the flyback interval, ~6 % of a field
 * (§12.1): 999 cycles at 60 Hz. §16 lists this as unconfirmed; it is a
 * percentage so that it follows the field rate if that changes. */
#define ATOM_FLYBACK_PERCENT    6u

/* ---- Audio (design.md §9.2, §9.4) ------------------------------------ */

#define ATOM_PWM_TOP         2047u  /* 11 bits, 73.2 kHz carrier          */
#define ATOM_PWM_OVERSAMPLE     2u  /* each frame written twice           */

#define ATOM_PCM_QUEUE_LEN   1024u  /* SPSC, ~28 ms                       */
#define ATOM_PCM_QUEUE_START  768u  /* start streaming at this depth      */

/* Power-of-two AND aligned, with the hardware read wrap
 * (hardware-notes.md §5.3). At oversample 2 a frame is two slots. */
#define ATOM_DMA_FRAMES_PER_HALF 128u
#define ATOM_DMA_SLOTS_PER_HALF  (ATOM_DMA_FRAMES_PER_HALF * ATOM_PWM_OVERSAMPLE)
#define ATOM_DMA_RING_SLOTS      (ATOM_DMA_SLOTS_PER_HALF * 2u)

/* ---- Keyboard (design.md §10) ---------------------------------------- */

#define ATOM_KEY_COLS          10u
#define ATOM_KEY_ROWS           6u
#define ATOM_KEY_EVENT_QUEUE   64u  /* southbridge FIFO holds 31 (§6.2)   */
#define ATOM_KEY_HELD_MAX       8u  /* keys down at once (§10.2)          */

/* Replay pacing for keymatrix_field (§10.2). A press and its release
 * can arrive in one 30 Hz poll, and the MOS cannot take keys that fast:
 * OSRDCH (#FE94) waits for every key to be up, then six fields (#FB8A),
 * and only then samples the matrix. So a replayed key must still be
 * down at least seven fields after the previous release: MIN + GAP >= 7,
 * with GAP >= 1 so the MOS sees all keys up. test_boot measured the
 * edge — 4 + 3 types, 3 + 3 and 4 + 2 drop characters — and these
 * leave a field of margin. Counted in fields, so it holds at 50 Hz. */
#define ATOM_KEY_MIN_FIELDS     6u
#define ATOM_KEY_GAP_FIELDS     2u

/* ---- Tape (design.md §11) -------------------------------------------- */

#define ATOM_ATM_NAME_LEN      16u
#define ATOM_PATH_MAX         128u

#endif /* PICO_ATOM_CONFIG_H */
