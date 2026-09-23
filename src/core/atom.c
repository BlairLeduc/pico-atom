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
    cfg->via_fitted     = true;    /* the MOS hangs without it (via6522.h) */
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
    /* Power-on RAM is zero-filled, so a fresh machine shows a screen of
     * '@' (glyph 0) until the MOS clears it. Real hardware comes up with
     * uninitialised RAM and shows a checkerboard of 0x00 and 0xFF whose
     * pattern depends on the RAM chips fitted; that is deliberately not
     * modelled, because it is per-machine noise rather than behaviour
     * any software can depend on. */
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

    i8255_reset(&m->ppi);
    atom_refresh_ppi_inputs(m);

    /* Port A is the printer port; bit 7 is BUSY, which the MOS polls
     * before every byte it prints. No printer is attached, so the line
     * reads ready and CTRL-B cannot hang the machine. */
    via6522_reset(&m->via);
    m->via.in_a = 0x7Fu;

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
        uint32_t c = m6502_step(m);
        done += c;
        if (m->cfg.via_fitted) {
            via6522_tick(&m->via, c);
            m6502_set_irq(&m->cpu, M6502_IRQ_VIA, via6522_irq(&m->via));
        }
    }
    return done;
}

/* Spend whatever the accumulator holds. A negative budget is debt from
 * the last instruction's overshoot and is paid off by running nothing. */
static uint32_t run_budget(atom_t *m) {
    if (m->budget <= 0) return 0;
    uint32_t done = atom_run(m, (uint32_t)m->budget);
    m->budget -= (int32_t)done;
    return done;
}

uint32_t atom_run_field(atom_t *m) {
    uint32_t flyback = atom_flyback_cycles(m);
    uint32_t done = 0;

    m->budget += (int32_t)(atom_cycles_per_field(m) - flyback);
    done += run_budget(m);

    /* Guest instructions must execute while FS is low, or the flag is
     * unobservable and the MOS's screen-writing loops spin for ever
     * (§12.1). */
    atom_field_sync(m, true);
    m->budget += (int32_t)flyback;
    done += run_budget(m);
    atom_field_sync(m, false);

    return done;
}


/* ---- keyboard, field sync and the VDG mode (design.md §4.1) --------- */

/* Rebuild port B and port C's input nibble from machine state. Port B's
 * row sense and the CTRL/SHIFT/REPT lines are all active low (§2.3). */
void atom_refresh_ppi_inputs(atom_t *m) {
    uint8_t col = i8255_kbd_column(&m->ppi);

    /* Columns 0-9 exist; anything above selects nothing and senses no
     * key, which is how the MOS's idle scan value behaves. */
    uint8_t rows = (col < ATOM_KEY_COLS) ? m->key_col[col] : 0u;

    uint8_t b = (uint8_t)(~rows & 0x3Fu);          /* bits 0-5, active low */
    if (!m->key_ctrl)  b |= 0x40u;                 /* bit 6, active low    */
    if (!m->key_shift) b |= 0x80u;                 /* bit 7, active low    */
    m->ppi.in_b = b;

    uint8_t c = m->ppi.in_c & 0x0Fu;
    if (!m->key_rept)   c |= I8255_IN_C_REPT;      /* active low           */
    if (!m->in_flyback) c |= I8255_IN_C_FS;
    /* Cassette inputs stay where the tape decoder left them (M8). */
    c |= (uint8_t)(m->ppi.in_c & (I8255_IN_C_CASSETTE_TONE |
                                  I8255_IN_C_CASSETTE_DATA));
    m->ppi.in_c = c;
}

void atom_key_set(atom_t *m, uint8_t row, uint8_t col, bool down) {
    if (row >= ATOM_KEY_ROWS || col >= ATOM_KEY_COLS) return;
    uint8_t bit = (uint8_t)(1u << row);
    if (down) m->key_col[col] |= bit;
    else      m->key_col[col] = (uint8_t)(m->key_col[col] & ~bit);
    atom_refresh_ppi_inputs(m);
}

void atom_key_mods(atom_t *m, bool shift, bool ctrl, bool rept) {
    m->key_shift = shift;
    m->key_ctrl  = ctrl;
    m->key_rept  = rept;
    atom_refresh_ppi_inputs(m);
}

void atom_field_sync(atom_t *m, bool in_flyback) {
    m->in_flyback = in_flyback;
    atom_refresh_ppi_inputs(m);
}

uint8_t atom_vdg_mode(const atom_t *m) {
    return mc6847_pack_mode(i8255_vdg_nibble(&m->ppi), i8255_css(&m->ppi));
}

bool atom_speaker(const atom_t *m) {
    return i8255_speaker(&m->ppi);
}
