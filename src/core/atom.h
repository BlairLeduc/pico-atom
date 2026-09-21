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

#include "config.h"
#include "i8255.h"
#include "m6502.h"
#include "mc6847.h"

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
    bool via_fitted;      /* 6522 at #B800 */
    bool atomdos;         /* 8271 FDC at #0A00 — steals 4 bytes of page #0A */
    unsigned field_hz;    /* 50 or 60; §16 medium confidence, so configurable */

    /* Which port A bit carries A/G. §16 marks this medium confidence and
     * the design document states it both ways (§2.3 vs §2.4), so it is
     * configuration until the circuit diagram settles it. */
    vdg_bit_order_t vdg_bit_order;
} atom_config_t;

typedef struct atom_s {
    m6502_t  cpu;
    i8255_t  ppi;
    mc6847_t vdg;

    /* Keyboard matrix: one bit per row, per column, 1 = key down. The
     * cell assignments are §16's low-confidence item and live in the
     * keymap, not here; this is only the mechanism. */
    uint8_t key_col[ATOM_KEY_COLS];
    bool    key_shift, key_ctrl, key_rept;

    bool in_flyback;      /* drives FS, port C bit 7 */

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

/* Cycles in one field, from the configured field rate (§12.1).
 * atom_init sanitises field_hz, but cfg is public and callers can reach
 * in afterwards, so the divide is guarded here too — it costs one
 * compare per field, not per instruction. */
static inline uint32_t atom_cycles_per_field(const atom_t *m) {
    unsigned hz = m->cfg.field_hz ? m->cfg.field_hz : ATOM_FIELD_HZ_DEFAULT;
    return (uint32_t)(ATOM_CPU_HZ / hz);
}

#endif /* PICO_ATOM_ATOM_H */
