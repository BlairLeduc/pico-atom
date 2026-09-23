/* bus.c — the slow path behind the page table (design.md §7).
 *
 * I/O is decoded by mask, not equality, because the Atom decodes
 * #B000-#BFFF partially and the 8255 mirrors every four bytes (§7.3).
 * Some software relies on a mirror.
 */

#include "bus.h"

#include "hot.h"
#include "i8255.h"
#include "i8271.h"
#include "via6522.h"

#define IS_8255(a)   (((a) & 0xFC00u) == 0xB000u)
#define IS_EXPAN(a)  (((a) & 0xFC00u) == 0xB400u)
#define IS_VIA(a)    (((a) & 0xFC00u) == 0xB800u)
/* #0A00-#0A02 are the 8271's registers on its A1-A0, and #0A04 its data
 * register on DACK: the four addresses the DOS ROM touches. A2 as DACK
 * gives eight bytes; that the rest of page #0A is RAM is §7.1's. */
#define IS_FDC(a)    (((a) & 0xFFF8u) == 0x0A00u)
#define FDC_REG(a)   ((uint8_t)(((a) & 4u) ? I8271_REG_DATA : ((a) & 3u)))

#define FDC_PAGE     0x0Au

uint8_t ATOM_HOT1(bus_read_slow)(atom_t *m, uint16_t a) {
    /* Page #0A, AtomDOS enabled: 8 bytes of FDC, 248 bytes of ordinary
     * RAM. This page has no fast path precisely so that this split can
     * happen — see the note in atom_init (§7.1). */
    if ((a >> 8) == FDC_PAGE && m->cfg.atomdos) {
        if (IS_FDC(a)) {
            /* Reading the result or the data drops INT, and NMI with it. */
            uint8_t v = i8271_read(&m->fdc, FDC_REG(a), m->cpu.cycles);
            atom_fdc_int(m);
            m->open_bus = v;
            return v;
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
            /* The cassette inputs are brought up to date only when they
             * can be seen (cassette.h, §11.3). */
            if (reg == 2u) atom_cassette_sync(m);
            uint8_t v = i8255_read(&m->ppi, reg);
            m->open_bus = v;
            return v;
        }
        if (IS_VIA(a) && m->cfg.via_fitted) {
            /* Reading T1C-L or T2C-L clears a flag, so the IRQ line can
             * drop on a read. */
            uint8_t v = via6522_read(&m->via, (uint8_t)(a & 15u));
            m6502_set_irq(&m->cpu, M6502_IRQ_VIA, via6522_irq(&m->via));
            m->open_bus = v;
            return v;
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

void ATOM_HOT1(bus_write_slow)(atom_t *m, uint16_t a, uint8_t v) {
    if ((a >> 8) == FDC_PAGE && m->cfg.atomdos) {
        if (IS_FDC(a)) {
            i8271_write(&m->fdc, FDC_REG(a), v, m->cpu.cycles);
            atom_fdc_int(m);
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
             * port B's row sense must be recomputed. A control-register
             * write can move port C's direction or clear the latches, so
             * it counts too.
             *
             * The mode needs no hook here: atom_vdg_mode() reads the
             * five bits off the latches when the field is snapshotted,
             * and the presenter compares that byte — not the whole port —
             * against what it last drew (§8.4), so a scan does not look
             * like a mode change. */
            if (reg == 0u || reg == 3u) atom_refresh_ppi_inputs(m);

            /* Port C and the BSR path both reach the speaker bit, and a
             * mode-set write clears it (§9.3). */
            if (reg >= 2u) atom_speaker_written(m);
            return;
        }
        if (IS_VIA(a) && m->cfg.via_fitted) {
            via6522_write(&m->via, (uint8_t)(a & 15u), v);
            m6502_set_irq(&m->cpu, M6502_IRQ_VIA, via6522_irq(&m->via));
            return;
        }
        return;
    }

    /* ROM and unpopulated space: the write is discarded. */
    (void)v;
}
