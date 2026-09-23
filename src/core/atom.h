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
#include "cassette.h"
#include "config.h"
#include "i8255.h"
#include "m6502.h"
#include "mc6847.h"
#include "tape.h"
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
    bool upper_ram;       /* #4000-#7FFF, the 32 KiB machine games assume */
    bool video;           /* #8000-#97FF */
    bool video_aperture;  /* #9800-#9FFF, absent on a stock machine */
    bool via_fitted;      /* 6522 at #B800; the MOS needs it (via6522.h) */
    bool atomdos;         /* 8271 FDC at #0A00 — steals 4 bytes of page #0A */
    bool tape_traps;      /* serve OSLOAD/OSSAVE from files (tape.h, §11.2) */
    bool tape_cues;       /* the deck follows the MOS's PLAY TAPE (§11.3) */
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

    /* Tape phase 1: the OSLOAD/OSSAVE request the CPU is stalled on, if
     * any (tape.h, §11.2). */
    tape_t tape;

    /* Tape phase 2: a UEF image played into port C bits 4-5 at signal
     * level (cassette.h, §11.3). While one is in the deck the OSLOAD
     * trap stands aside, since the program is on the tape. */
    cassette_t cas;

    uint8_t ram[ATOM_ADDR_SPACE];
    page_t  page[ATOM_PAGE_COUNT];
    uint8_t page_flags[ATOM_PAGE_COUNT];

    /* Open bus is modelled as the last value on the bus, not 0xFF (§7.2). */
    uint8_t open_bus;

    atom_config_t cfg;

    /* Cycle debt carried between field-sized slices (§12.1). */
    int32_t  budget;

    /* Instructions executed, for §12.3's host cycles per guest
     * instruction. A counter, not machine state: snapshots leave it. */
    uint64_t instructions;
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

/* Copy a whole machine. The page table points into ram[], so a plain
 * struct assignment leaves the copy reading and writing the original's
 * memory; this moves the pointers across with the bytes. */
void atom_copy(atom_t *dst, const atom_t *src);

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

/* The cassette (cassette.h), on the guest clock. The image is the
 * caller's and must outlive the insertion. */
bool atom_cassette_insert(atom_t *m, const uint8_t *img, size_t len);
void atom_cassette_eject(atom_t *m);
void atom_cassette_play(atom_t *m, bool on);
void atom_cassette_rewind(atom_t *m);
static inline bool atom_cassette_playing(const atom_t *m) { return m->cas.playing; }

/* Bring port C's cassette inputs, bits 4 and 5, up to the current cycle.
 * bus.c calls this before every read of port C: that is the only time
 * they can be seen, so they are not clocked per instruction. */
void atom_cassette_sync_slow(atom_t *m);

/* The MOS polls port C for FS in its tightest loops (§12.1), so the
 * common case is inline and changes nothing: neither bit 4 nor the tape
 * is due to change yet, so both bits are what the last sync left. One
 * subtract and a branch; anything else takes the call. The compare is
 * modulo 2^32, which atom_run_field keeps honest by syncing once a
 * field whether or not the guest reads. */
static inline void atom_cassette_sync(atom_t *m) {
    if (__builtin_expect((int32_t)((uint32_t)m->cpu.cycles - m->cas.ref_next) >= 0, 0))
        atom_cassette_sync_slow(m);
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
