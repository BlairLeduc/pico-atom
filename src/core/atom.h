/* atom.h — the guest machine (design.md §4.1, §7).
 *
 * All state is here or in statically sized buffers from config.h; core/
 * performs no dynamic allocation, which is what makes the §5 budget a
 * link-time fact.
 */
#ifndef PICO_ATOM_ATOM_H
#define PICO_ATOM_ATOM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "beeper.h"
#include "config.h"
#include "i8255.h"
#include "m6502.h"
#include "mc6847.h"
#include "via6522.h"

/* Page descriptor flags (§7.1). */
#define PAGE_ROM   0x01u  /* writes ignored                            */
#define PAGE_IO    0x02u  /* reads and writes take the slow path       */
#define PAGE_VRAM  0x04u  /* informational; VRAM needs no write hook   */
#define PAGE_OPEN  0x08u  /* unpopulated: reads return the open bus    */

/* Exactly two pointers, which is the 256 x 2 x 4 B the §5 budget
 * allows. The flags are a separate byte array rather than a third
 * struct field so the descriptor does not pad out to twelve bytes; only
 * the slow path reads them, so the extra indirection is free. */
typedef struct {
    uint8_t *read;    /* NULL -> slow path            */
    uint8_t *write;   /* NULL -> not writable, or I/O */
} page_t;

/* Which RAM blocks are populated, and which optional devices are fitted.
 * Configuration, not code (§7.2). */
typedef struct {
    bool block_zero;      /* #0000-#03FF */
    bool text_space;      /* #0400-#3FFF */
    bool video;           /* #8000-#97FF */
    bool video_aperture;  /* #9800-#9FFF, absent on a stock machine */
    bool via_fitted;      /* 6522 at #B800; the MOS needs it (via6522.h) */
    bool atomdos;         /* 8271 FDC at #0A00 — steals 4 bytes of page #0A */
    unsigned field_hz;    /* 50 or 60; §16 medium confidence, so configurable */
} atom_config_t;

typedef struct atom_s {
    m6502_t  cpu;
    i8255_t  ppi;
    via6522_t via;       /* inert unless cfg.via_fitted */

    /* No mc6847_t here. The guest's video state is VRAM plus the five
     * mode bits, and both are read out through atom_vram() and
     * atom_vdg_mode() at snapshot time. The renderer, and its 8 KiB LUT,
     * belong to whoever presents — core 1 on the device (§4.2, §8.4) —
     * so core 0 never rebuilds a table the other core may be reading. */

    /* Keyboard matrix: one bit per row, per column, 1 = key down. The
     * cell assignments are §16's low-confidence item and live in the
     * keymap, not here; this is only the mechanism. */
    uint8_t key_col[ATOM_KEY_COLS];
    bool    key_shift, key_ctrl, key_rept;

    bool in_flyback;      /* drives FS, port C bit 7 */

    /* The loudspeaker, port C bit 2, integrated into PCM (§9.3). The
     * port drains it once per field with atom_audio_drain. */
    beeper_t beeper;

    uint8_t ram[ATOM_ADDR_SPACE];
    page_t  page[ATOM_PAGE_COUNT];
    uint8_t page_flags[ATOM_PAGE_COUNT];

    /* Open bus is modelled as the last value on the bus, not 0xFF (§7.2). */
    uint8_t open_bus;

    atom_config_t cfg;

    /* Cycle debt carried between field-sized slices (§12.1). */
    int32_t  budget;
} atom_t;

void atom_config_default(atom_config_t *cfg);

/* Wire up the page table from cfg and reset the CPU. */
void atom_init(atom_t *m, const atom_config_t *cfg);
void atom_reset(atom_t *m);

/* Run at least `cycles` guest cycles, finishing whole instructions.
 * Returns the cycles actually run, which the caller carries as debt. */
uint32_t atom_run(atom_t *m, uint32_t cycles);

/* Run one field, split at the flyback boundary (§12.1): the active part
 * with FS high, then FS low and the flyback part, then FS high again.
 * The guest executes on both sides of the edge, so a program polling
 * #B002 bit 7 sees the low state and escapes. Debt carries in
 * m->budget across both halves and across fields. Returns the cycles
 * actually run. */
uint32_t atom_run_field(atom_t *m);

/* Load a ROM image at a guest address, marking its pages PAGE_ROM. */
bool atom_load_rom(atom_t *m, uint16_t addr, const uint8_t *data, size_t len);

/* Make a region plain read/write RAM. Used by the host tests, which need a
 * bare 64 KiB machine for the Dormann and Clark suites. */
void atom_map_ram(atom_t *m, uint16_t addr, uint32_t len);

/* ---- the seam every test in §15 exercises (design.md §4.1) ---------- */

/* Recompute the 8255's input sources from machine state. Called for
 * you by the setters below and after every port A write. */
void atom_refresh_ppi_inputs(atom_t *m);

void atom_key_set(atom_t *m, uint8_t row, uint8_t col, bool down);
void atom_key_mods(atom_t *m, bool shift, bool ctrl, bool rept);
void atom_field_sync(atom_t *m, bool in_flyback);

static inline const uint8_t *atom_vram(const atom_t *m) {
    return &m->ram[ATOM_VRAM_BASE];
}

/* A/G, GM2:0 and CSS packed into five bits (§8.1). */
uint8_t atom_vdg_mode(const atom_t *m);

/* Is the loudspeaker bit currently high? Port C bit 2 (§9.1). */
bool atom_speaker(const atom_t *m);

/* A port C write may have moved the speaker bit; bus.c calls this after
 * every write to #B002 or #B003 (§9.3). */
void atom_speaker_written(atom_t *m);

/* The sample rate is rate_num / rate_den Hz, as a fraction so that the
 * cadence is exact (§9.2). atom_init starts at the nominal
 * ATOM_AUDIO_RATE_NUM / ATOM_AUDIO_RATE_DEN; the port passes the rate
 * its clocks actually give. */
void atom_audio_set_rate(atom_t *m, uint32_t rate_num, uint32_t rate_den);

/* Move up to `max` samples of signed 16-bit mono out of the machine,
 * oldest first. Samples are produced as the guest runs, one per 27.31
 * guest cycles at the nominal rate; atom_run leaves every sample that
 * ended before its last instruction ready to drain (§4.1). */
size_t atom_audio_drain(atom_t *m, int16_t *dst, size_t max);

/* Cycles in one field, from the configured field rate (§12.1).
 * atom_init sanitises field_hz, but cfg is public and callers can reach
 * in afterwards, so the divide is guarded here too — it costs one
 * compare per field, not per instruction. */
static inline uint32_t atom_cycles_per_field(const atom_t *m) {
    unsigned hz = m->cfg.field_hz ? m->cfg.field_hz : ATOM_FIELD_HZ_DEFAULT;
    return (uint32_t)(ATOM_CPU_HZ / hz);
}

static inline uint32_t atom_flyback_cycles(const atom_t *m) {
    return atom_cycles_per_field(m) * ATOM_FLYBACK_PERCENT / 100u;
}

#endif /* PICO_ATOM_ATOM_H */
