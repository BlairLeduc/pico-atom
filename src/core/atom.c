/* atom.c — machine assembly: page table, run loop (design.md §4.1, §7). */

#include "atom.h"

#include <string.h>

#include "bus.h"

void atom_config_default(atom_config_t *cfg) {
    /* A fully expanded machine by default (§7.2). */
    cfg->block_zero     = true;
    cfg->text_space     = true;
    cfg->video          = true;
    cfg->video_aperture = false;   /* absent on a stock machine */
    cfg->via_fitted     = false;
    cfg->atomdos        = false;
    cfg->field_hz       = ATOM_FIELD_HZ_DEFAULT;
}

static void map_open(atom_t *m, unsigned first_page, unsigned last_page) {
    for (unsigned p = first_page; p <= last_page; p++) {
        m->page[p].read  = NULL;
        m->page[p].write = NULL;
        m->page_flags[p] = PAGE_OPEN;
    }
}

static void map_rw(atom_t *m, unsigned first_page, unsigned last_page, uint8_t flags) {
    for (unsigned p = first_page; p <= last_page; p++) {
        m->page[p].read  = &m->ram[p * ATOM_PAGE_SIZE];
        m->page[p].write = &m->ram[p * ATOM_PAGE_SIZE];
        m->page_flags[p] = flags;
    }
}

static void map_io(atom_t *m, unsigned first_page, unsigned last_page) {
    for (unsigned p = first_page; p <= last_page; p++) {
        m->page[p].read  = NULL;
        m->page[p].write = NULL;
        m->page_flags[p] = PAGE_IO;
    }
}

void atom_init(atom_t *m, const atom_config_t *cfg) {
    memset(m, 0, sizeof(*m));
    m->cfg = *cfg;

    /* The field rate is configuration rather than a constant (§16, §18),
     * which means it can arrive wrong. Zero would divide by zero in
     * atom_cycles_per_field, so it is sanitised once, here, rather than
     * defended against at every use. */
    if (m->cfg.field_hz == 0) m->cfg.field_hz = ATOM_FIELD_HZ_DEFAULT;
    m->open_bus = 0xFFu;

    map_open(m, 0x00u, 0xFFu);

    if (m->cfg.block_zero) map_rw(m, 0x00u, 0x03u, 0);
    if (m->cfg.text_space) map_rw(m, 0x04u, 0x3Fu, 0);
    if (m->cfg.video)      map_rw(m, 0x80u, 0x97u, PAGE_VRAM);
    if (m->cfg.video_aperture) map_rw(m, 0x98u, 0x9Fu, PAGE_VRAM);

    /* #B000-#BFFF is I/O throughout; the mask decode in bus.c sorts out
     * which device, and the partial decoding means the 8255 mirrors. */
    map_io(m, 0xB0u, 0xBFu);

    /* A page-granular fast path cannot express a sub-page device, and the
     * Atom has one: with AtomDOS the 8271 sits at #0A00-#0A03 inside a
     * page that is otherwise RAM. If page #0A kept a non-NULL write those
     * four addresses would take the fast RAM store and the FDC would
     * never be reached — silently (§7.1). */
    if (m->cfg.atomdos) map_io(m, 0x0Au, 0x0Au);

    /* ROM sockets stay unpopulated until an image is supplied; the build
     * ships no ROM binaries (§1, §11.1). */

    m6502_init(&m->cpu);
    m->budget = 0;
    atom_reset(m);
}

void atom_reset(atom_t *m) {
    m->cpu.reset_pending = false;
    m6502_reset(&m->cpu, m);
}

bool atom_load_rom(atom_t *m, uint16_t addr, const uint8_t *data, size_t len) {
    if (len == 0) return false;
    if ((addr % ATOM_PAGE_SIZE) != 0) return false;
    if ((len % ATOM_PAGE_SIZE) != 0) return false;
    if ((size_t)addr + len > ATOM_ADDR_SPACE) return false;

    memcpy(&m->ram[addr], data, len);

    unsigned first = (unsigned)(addr / ATOM_PAGE_SIZE);
    unsigned count = (unsigned)(len / ATOM_PAGE_SIZE);
    for (unsigned p = first; p < first + count; p++) {
        m->page[p].read  = &m->ram[p * ATOM_PAGE_SIZE];
        m->page[p].write = NULL;          /* writes ignored */
        m->page_flags[p] = PAGE_ROM;
    }
    return true;
}

void atom_map_ram(atom_t *m, uint16_t addr, uint32_t len) {
    /* A zero length maps nothing. Without this, addr + len - 1 underflows
     * and an atom_map_ram(m, 0, 0) quietly maps the whole address space. */
    if (len == 0) return;

    uint32_t end = (uint32_t)addr + len;
    if (end > ATOM_ADDR_SPACE) end = ATOM_ADDR_SPACE;

    unsigned first = (unsigned)(addr / ATOM_PAGE_SIZE);
    unsigned last  = (unsigned)((end - 1u) / ATOM_PAGE_SIZE);
    map_rw(m, first, last, 0);
}

uint32_t atom_run(atom_t *m, uint32_t cycles) {
    uint32_t done = 0;
    /* Whole instructions until at least `cycles` have elapsed; the caller
     * carries the overshoot forward as debt (§6.2, §12.1). */
    while (done < cycles) {
        done += m6502_step(m);
    }
    return done;
}
