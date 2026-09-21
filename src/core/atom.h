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
#include "m6502.h"

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
} atom_config_t;

typedef struct atom_s {
    m6502_t cpu;

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

/* Cycles in one field, from the configured field rate (§12.1).
 * atom_init sanitises field_hz, but cfg is public and callers can reach
 * in afterwards, so the divide is guarded here too — it costs one
 * compare per field, not per instruction. */
static inline uint32_t atom_cycles_per_field(const atom_t *m) {
    unsigned hz = m->cfg.field_hz ? m->cfg.field_hz : ATOM_FIELD_HZ_DEFAULT;
    return (uint32_t)(ATOM_CPU_HZ / hz);
}

#endif /* PICO_ATOM_ATOM_H */
