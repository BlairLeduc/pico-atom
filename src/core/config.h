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

/* 60 Hz on every Atom, UK machines included (§16, confirmed): the
 * MC6847 is an NTSC part, and software times itself on it — BASIC's
 * WAIT is one field sync, 1/60 s. A constant, not configuration. */
#define ATOM_FIELD_HZ          60u
#define ATOM_CYCLES_PER_FIELD  (ATOM_CPU_HZ / ATOM_FIELD_HZ)   /* 16666 */

/* A field is the MC6847's 262 lines (§12.1): 192 active; then FS (port
 * C bit 7) low for 32, the bottom border and retrace; then 38 more with
 * FS high, vertical blank and the top border, before the next active
 * line. The line counts are the datasheet's, figures 8 and 13 (§16). A
 * line is 63.6 cycles, so each part is scaled from the field, not
 * counted in whole lines. */
#define ATOM_FIELD_LINES      262u
#define ATOM_ACTIVE_LINES     192u
#define ATOM_FS_LOW_LINES      32u
#define ATOM_BLANK_LINES     (ATOM_FIELD_LINES - ATOM_ACTIVE_LINES - ATOM_FS_LOW_LINES)
#define ATOM_FS_LOW_CYCLES   (ATOM_CYCLES_PER_FIELD * ATOM_FS_LOW_LINES / ATOM_FIELD_LINES) /* 2035 */
#define ATOM_BLANK_CYCLES    (ATOM_CYCLES_PER_FIELD * ATOM_BLANK_LINES / ATOM_FIELD_LINES)  /* 2417 */
#define ATOM_ACTIVE_CYCLES   (ATOM_CYCLES_PER_FIELD - ATOM_FS_LOW_CYCLES - ATOM_BLANK_CYCLES)

/* ---- Audio (design.md §9.2, §9.4) ------------------------------------ */

#define ATOM_PWM_TOP         2047u  /* 11 bits, 73.2 kHz carrier          */
#define ATOM_PWM_OVERSAMPLE     2u  /* each frame written twice           */

/* The nominal sample rate as a fraction, 150,000,000 / 4096 Hz (§9.2).
 * The port recomputes it from clock_get_hz(clk_sys) and hands the real
 * one to atom_audio_set_rate; this is what a host build runs at. */
#define ATOM_AUDIO_RATE_NUM  150000000u
#define ATOM_AUDIO_RATE_DEN  ((ATOM_PWM_TOP + 1u) * ATOM_PWM_OVERSAMPLE)

/* Samples the core holds between drains. One field is 611; the port
 * drains after every field (§12.2). */
#define ATOM_AUDIO_BUF_LEN   1024u

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
 * leave a field of margin. */
#define ATOM_KEY_MIN_FIELDS     6u
#define ATOM_KEY_GAP_FIELDS     2u

/* Game keymaps (§10.5): overlays on the standard map, built in or read
 * off the card. A layout rebinds a handful of keys, so these are loose. */
#define ATOM_KEYMAP_LAYOUTS     8u  /* built-in and card layouts together */
#define ATOM_KEYMAP_BINDINGS   16u  /* bindings per layout                */
#define ATOM_KEYMAP_TAPES       4u  /* ATM names on a layout's tapes line */
#define ATOM_KEYMAP_NAME_LEN   16u  /* a layout's name, as the menu shows */
#define ATOM_KEYMAP_FILE_MAX 1024u  /* the largest .map file read         */

/* /atom/pico-atom.cfg (§11.7): a dozen settings and their comments. */
#define ATOM_SETTINGS_FILE_MAX 2048u

/* ---- Tape (design.md §11) -------------------------------------------- */

#define ATOM_ATM_NAME_LEN      16u
#define ATOM_PATH_MAX         128u
#define ATOM_TAPE_LIST_MAX     48u  /* tape files the menu lists (§13)     */

/* Port C bit 4, the 2.4 kHz reference the MOS times each recorded bit
 * against (#FCD8), in guest cycles per period. §16, medium: 4 MHz / 1664
 * is 2403.8 Hz, which MAME's Atom driver also uses; the ROM reads the
 * tape by its own loop timing, so only saving depends on it. */
#define ATOM_CASSETTE_REF_CYCLES 416u

/* The largest UEF image the deck holds, decompressed (§11.3). The
 * Atom's 300 baud makes an image about 1.1x the data it carries plus
 * its tones, so this is room for the biggest multi-part games. */
#define ATOM_UEF_MAX         65536u

/* ---- Disc (design.md §11.4) ------------------------------------------ */

/* Two drive select lines on the 8271; drives 2 and 3 are their second
 * sides, which AtomDOS picks with the side-select output (#E78E). */
#define ATOM_FDC_DRIVES          2u

/* Acorn's FM format: 256-byte sectors, ten to a track. The FDC asks
 * the port for at most a track at a time, so this is its buffer. */
#define ATOM_DISC_SECTOR_LEN   256u
#define ATOM_DISC_SECTORS       10u
#define ATOM_DISC_TRACKS_MAX    80u
#define ATOM_FDC_BUF_LEN   (ATOM_DISC_SECTORS * ATOM_DISC_SECTOR_LEN)

/* Guest cycles, at 1 MHz. FM at 125 kbit/s is a byte every 64 us: the
 * DOS's NMI handler (#E87B) takes 52 cycles a byte, so it keeps up. A
 * 300 rpm revolution is 200 ms, a tenth of it per sector. Stepping and
 * settling are not taken from SPECIFY's parameters, whose units are
 * unconfirmed; nothing the DOS does depends on them. */
#define ATOM_FDC_BYTE_CYCLES     64u
#define ATOM_FDC_SECTOR_CYCLES 20000u
#define ATOM_FDC_STEP_CYCLES    6000u
#define ATOM_FDC_SETTLE_CYCLES 15000u
#define ATOM_FDC_CMD_CYCLES      200u   /* command to its first action */

#define ATOM_DISC_LIST_MAX      48u    /* disc images the menu lists (§13) */

#endif /* PICO_ATOM_CONFIG_H */
