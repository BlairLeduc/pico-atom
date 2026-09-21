/* bus.c — the slow path behind the page table (design.md §7).
 *
 * I/O is decoded by mask, not equality, because the Atom decodes
 * #B000-#BFFF partially and the 8255 mirrors every four bytes (§7.3).
 * Some software relies on a mirror.
 */

#include "bus.h"

#include "i8255.h"
#include "mc6847.h"

#define IS_8255(a)   (((a) & 0xFC00u) == 0xB000u)
#define IS_EXPAN(a)  (((a) & 0xFC00u) == 0xB400u)
#define IS_VIA(a)    (((a) & 0xFC00u) == 0xB800u)
#define IS_FDC(a)    (((a) & 0xFFFCu) == 0x0A00u)

#define FDC_PAGE     0x0Au

uint8_t bus_read_slow(atom_t *m, uint16_t a) {
    /* Page #0A, AtomDOS enabled: 4 bytes of FDC, 252 bytes of ordinary
     * RAM. This page has no fast path precisely so that this split can
     * happen — see the note in atom_init (§7.1). */
    if ((a >> 8) == FDC_PAGE && m->cfg.atomdos) {
        if (IS_FDC(a)) {
            /* M9: fdc_read(m, a & 3). */
            return m->open_bus;
        }
        uint8_t v = m->ram[a];
        m->open_bus = v;
        return v;
    }

    if (m->page_flags[a >> 8] & PAGE_IO) {
        if (IS_8255(a)) {
            /* Decoded by mask, so the chip mirrors every four bytes
             * through its block and some software relies on it (§7.3).
             * #B003 is write-only on the real part, so it is not driven. */
            uint8_t reg = (uint8_t)(a & 3u);
            if (!i8255_reg_drives_bus(reg)) return m->open_bus;
            uint8_t v = i8255_read(&m->ppi, reg);
            m->open_bus = v;
            return v;
        }
        if (IS_VIA(a) && m->cfg.via_fitted) {
            /* M9: via6522_read(&m->via, a & 15). */
            return m->open_bus;
        }
        if (IS_EXPAN(a)) {
            /* M6+: expansion / printer port. */
            return m->open_bus;
        }
        return m->open_bus;
    }

    /* Unpopulated. Open bus is the last value on the bus, not 0xFF
     * (§7.2) — it costs one field in the struct and occasionally
     * matters. */
    return m->open_bus;
}

void bus_write_slow(atom_t *m, uint16_t a, uint8_t v) {
    if ((a >> 8) == FDC_PAGE && m->cfg.atomdos) {
        if (IS_FDC(a)) {
            /* M9: fdc_write(m, a & 3, v). */
            return;
        }
        m->ram[a] = v;
        return;
    }

    if (m->page_flags[a >> 8] & PAGE_IO) {
        if (IS_8255(a)) {
            uint8_t reg = (uint8_t)(a & 3u);
            i8255_write(&m->ppi, reg, v);

            /* Port A carries the keyboard column in its low nibble and
             * the VDG mode in its high one, so *every keyboard scan
             * writes the video mode too* (§2.3). The column change means
             * port B's row sense must be recomputed; the mode nibble is
             * compared rather than the whole port, so a scan does not
             * look like a mode change. A control-register write can move
             * port C's direction or clear the latches, so it counts as
             * both. */
            if (reg == 0u || reg == 3u) atom_refresh_ppi_inputs(m);
            if (reg == 0u || reg == 2u || reg == 3u)
                mc6847_set_mode(&m->vdg, atom_vdg_mode(m));
            return;
        }
        if (IS_VIA(a) && m->cfg.via_fitted) {
            /* M9: via6522_write(&m->via, a & 15, v). */
            return;
        }
        return;
    }

    /* ROM and unpopulated space: the write is discarded. */
    (void)v;
}
