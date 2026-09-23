/* test_i8255.c — the INS8255 PPI (design.md §2.3, §15.1).
 *
 * The three things §2.3 says the model must get right: port C nibble
 * separation, the bit set/reset path, and the mode nibble being
 * independent of the keyboard column nibble in port A.
 */

#include <string.h>

#include "atom.h"
#include "bus.h"
#include "i8255.h"
#include "m6502.h"
#include "mc6847.h"
#include "test_util.h"

static atom_t g_machine;

static atom_t *machine(void) {
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&g_machine, &cfg);
    return &g_machine;
}

/* The Atom's usual programming: port A output, port B input, port C
 * upper input, port C lower output. */
#define ATOM_CTRL_WORD 0x8Au

int main(void) {
    /* ---- the control word decodes to the directions it names -------- */
    {
        i8255_t p;
        i8255_reset(&p);
        i8255_write(&p, 3, ATOM_CTRL_WORD);
        CHECK((p.control & I8255_CTRL_A_INPUT) == 0, "0x8A makes port A an output");
        CHECK((p.control & I8255_CTRL_B_INPUT) != 0, "0x8A makes port B an input");
        CHECK((p.control & I8255_CTRL_C_HI_INPUT) != 0, "0x8A makes port C upper an input");
        CHECK((p.control & I8255_CTRL_C_LO_INPUT) == 0, "0x8A makes port C lower an output");
    }

    /* ---- a mode-set write clears the output latches ----------------- */
    {
        i8255_t p;
        i8255_reset(&p);
        i8255_write(&p, 3, ATOM_CTRL_WORD);
        i8255_write(&p, 0, 0xA5);
        i8255_write(&p, 2, 0x0F);
        CHECK(p.out_a == 0xA5 && p.out_c == 0x0F, "latches should hold what was written");
        i8255_write(&p, 3, ATOM_CTRL_WORD);
        CHECK(p.out_a == 0 && p.out_b == 0 && p.out_c == 0,
              "a mode-set write clears the output latches");
    }

    /* ---- port C is two halves, and a read mixes them ----------------- */
    {
        i8255_t p;
        i8255_reset(&p);
        i8255_write(&p, 3, ATOM_CTRL_WORD);
        p.in_c = 0xA0;               /* input nibble: FS high, REPT high */
        i8255_write(&p, 2, 0x0F);    /* output nibble: all four bits set */

        uint8_t got = i8255_read(&p, 2);
        CHECK(got == 0xAF, "port C should read input upper | output lower, got 0x%02X", got);

        /* Writing to port C must not disturb what the inputs report. */
        p.in_c = 0x80;
        CHECK((i8255_read(&p, 2) & 0xF0u) == 0x80,
              "the input nibble follows its source, not the latch");
    }

    /* A read-modify-write on port C must leave the output nibble — which
     * carries CSS and the speaker — intact. This is the sequence §2.3
     * warns corrupts the mode bits if the latches are not kept separate
     * from the input sources. */
    {
        i8255_t p;
        i8255_reset(&p);
        i8255_write(&p, 3, ATOM_CTRL_WORD);
        i8255_write(&p, 2, 0x08);    /* CSS set */
        p.in_c = 0xF0;               /* every input line high */

        uint8_t v = i8255_read(&p, 2);
        i8255_write(&p, 2, v);       /* the naive read-modify-write */
        CHECK(i8255_css(&p), "CSS must survive a read-modify-write on port C");
        CHECK((i8255_read(&p, 2) & 0x0Fu) == 0x08,
              "the output nibble must survive a read-modify-write");
    }

    /* ---- bit set/reset, the path the MOS uses ------------------------ */
    {
        i8255_t p;
        i8255_reset(&p);
        i8255_write(&p, 3, ATOM_CTRL_WORD);

        CHECK(!i8255_speaker(&p), "the speaker starts low");
        i8255_write(&p, 3, 0x05);            /* BSR: set bit 2   */
        CHECK(i8255_speaker(&p), "BSR should set the loudspeaker bit");
        i8255_write(&p, 3, 0x04);            /* BSR: reset bit 2 */
        CHECK(!i8255_speaker(&p), "BSR should reset the loudspeaker bit");

        i8255_write(&p, 3, 0x07);            /* BSR: set bit 3   */
        CHECK(i8255_css(&p), "BSR should set CSS");
        CHECK(!i8255_speaker(&p), "BSR must touch only the bit it names");

        /* Every bit position is reachable. */
        for (unsigned bit = 0; bit < 8u; bit++) {
            i8255_write(&p, 3, ATOM_CTRL_WORD);
            i8255_write(&p, 3, (uint8_t)((bit << 1) | 1u));
            CHECK(p.out_c == (uint8_t)(1u << bit),
                  "BSR set of bit %u gave 0x%02X", bit, p.out_c);
        }

        /* A BSR write must not be mistaken for a mode set. */
        i8255_write(&p, 3, ATOM_CTRL_WORD);
        i8255_write(&p, 0, 0x5A);
        i8255_write(&p, 3, 0x05);
        CHECK(p.out_a == 0x5A, "a BSR write must not clear the other latches");
        CHECK(p.control == ATOM_CTRL_WORD, "a BSR write must not become the control word");
    }

    /* ---- port A: column in the low nibble, mode in the high one ------ */
    {
        atom_t *m = machine();
        bus_write(m, 0xB003, ATOM_CTRL_WORD);

        /* Every keyboard scan writes the video mode too (§2.3), so the
         * mode must be compared by nibble, not by whole port. */
        bus_write(m, 0xB000, 0x90);
        uint8_t mode_before = atom_vdg_mode(m);
        for (uint8_t col = 0; col < 10u; col++) {
            bus_write(m, 0xB000, (uint8_t)(0x90u | col));
            CHECK(i8255_kbd_column(&m->ppi) == col,
                  "column %u should select column %u", col, col);
            CHECK(atom_vdg_mode(m) == mode_before,
                  "scanning column %u must not change the video mode", col);
        }

        /* And changing the mode nibble must not disturb the column. */
        bus_write(m, 0xB000, 0x05);
        CHECK(i8255_kbd_column(&m->ppi) == 5, "column should survive a mode change");
    }

    /* ---- port B: row sense, active low, per selected column ---------- */
    {
        atom_t *m = machine();
        bus_write(m, 0xB003, ATOM_CTRL_WORD);
        bus_write(m, 0xB000, 0x03);              /* select column 3 */

        CHECK((bus_read(m, 0xB001) & 0x3Fu) == 0x3F,
              "no key down should read all rows high");

        atom_key_set(m, 2, 3, true);             /* row 2, column 3 */
        CHECK((bus_read(m, 0xB001) & 0x3Fu) == (0x3Fu & ~0x04u),
              "a pressed key should pull its row low, got 0x%02X",
              bus_read(m, 0xB001) & 0x3Fu);

        /* The same key is invisible from a different column. */
        bus_write(m, 0xB000, 0x04);
        CHECK((bus_read(m, 0xB001) & 0x3Fu) == 0x3F,
              "column 4 should not see a key in column 3");

        bus_write(m, 0xB000, 0x03);
        atom_key_set(m, 2, 3, false);
        CHECK((bus_read(m, 0xB001) & 0x3Fu) == 0x3F, "releasing should restore the row");
    }

    /* ---- CTRL and SHIFT are separate lines, also active low ---------- */
    {
        atom_t *m = machine();
        bus_write(m, 0xB003, ATOM_CTRL_WORD);
        CHECK((bus_read(m, 0xB001) & 0xC0u) == 0xC0, "CTRL and SHIFT idle high");

        atom_key_mods(m, true, false, false);
        CHECK((bus_read(m, 0xB001) & 0x80u) == 0, "SHIFT should pull bit 7 low");
        CHECK((bus_read(m, 0xB001) & 0x40u) != 0, "CTRL should be unaffected");

        atom_key_mods(m, false, true, false);
        CHECK((bus_read(m, 0xB001) & 0x40u) == 0, "CTRL should pull bit 6 low");
        CHECK((bus_read(m, 0xB001) & 0x80u) != 0, "SHIFT should be unaffected");
    }

    /* ---- port C inputs: REPT active low, FS the flyback flag --------- */
    {
        atom_t *m = machine();
        bus_write(m, 0xB003, ATOM_CTRL_WORD);

        atom_field_sync(m, false);
        CHECK((bus_read(m, 0xB002) & 0x80u) != 0, "FS should be high outside flyback");
        atom_field_sync(m, true);
        CHECK((bus_read(m, 0xB002) & 0x80u) == 0, "FS should go low during flyback");

        atom_key_mods(m, false, false, false);
        CHECK((bus_read(m, 0xB002) & 0x40u) != 0, "REPT idles high");
        atom_key_mods(m, false, false, true);
        CHECK((bus_read(m, 0xB002) & 0x40u) == 0, "REPT pulls bit 6 low");
    }

    /* ---- the chip mirrors every four bytes through its block (§7.3) --
     *
     * The block is #B000-#B3FF, selected by (a & 0xFC00) == 0xB000 — not
     * the whole of #B000-#BFFF, which also holds the expansion port at
     * #B400 and the VIA at #B800. Each read below is preceded by a write
     * elsewhere so that a device which failed to drive the bus would
     * return a *different* open-bus value rather than accidentally
     * matching what we expect. */
    {
        atom_t *m = machine();
        bus_write(m, 0xB003, ATOM_CTRL_WORD);
        bus_write(m, 0xB000, 0x37);

        for (uint16_t a = 0xB000; a < 0xB400u; a = (uint16_t)(a + 4u)) {
            bus_write(m, 0x0000, 0x5A);     /* poison the open bus */
            uint8_t got = bus_read(m, a);
            CHECK(got == 0x37, "#%04X should mirror port A, got 0x%02X", a, got);
        }

        /* The mirror repeats every four bytes, so the register index is
         * the low two address bits. */
        bus_write(m, 0x0000, 0x5A);
        CHECK(bus_read(m, 0xB3FC) == 0x37, "#B3FC should be port A");
        bus_write(m, 0xB3FC, 0x42);
        CHECK(i8255_read(&m->ppi, 0) == 0x42, "a write through a mirror should land");

        /* And the block genuinely ends at #B400: the expansion port is a
         * different device, not another mirror. */
        bus_write(m, 0x0000, 0x5A);
        CHECK(bus_read(m, 0xB400) != 0x42,
              "#B400 is the expansion port, not an 8255 mirror");
        bus_write(m, 0xB400, 0x99);
        CHECK(i8255_read(&m->ppi, 0) == 0x42,
              "a write to the expansion port must not reach the 8255");
    }

    /* ---- the 6502's read-modify-write double write is harmless here -
     *
     * Every writable register on this chip is an idempotent latch, so the
     * NMOS double write cannot be distinguished from a single one. That
     * is worth asserting rather than assuming: it is the reason the
     * double-write behaviour has no test until the VIA arrives at M9. */
    {
        atom_t *m = machine();
        bus_write(m, 0xB003, ATOM_CTRL_WORD);
        bus_write(m, 0xB002, 0x05);
        m->ppi.in_c = 0xF0;

        /* ASL $B002: read, write back unmodified, write the result. */
        atom_map_ram(m, 0x1000, 0x0100);
        m->ram[0x1000] = 0x0E; m->ram[0x1001] = 0x02; m->ram[0x1002] = 0xB0;
        m->cpu.pc = 0x1000;
        m6502_step(m);
        uint8_t after_rmw = m->ppi.out_c;

        /* The same machine, reaching the same final value with one write. */
        atom_t *n = machine();
        bus_write(n, 0xB003, ATOM_CTRL_WORD);
        bus_write(n, 0xB002, 0x05);
        n->ppi.in_c = 0xF0;
        /* Through the bus, as the CPU reads it: the cassette inputs,
         * bits 4 and 5, are driven at the read (cassette.h). */
        bus_write(n, 0xB002, (uint8_t)(bus_read(n, 0xB002) << 1));

        CHECK(after_rmw == n->ppi.out_c,
              "an RMW on port C should land where a single write does: "
              "0x%02X vs 0x%02X", after_rmw, n->ppi.out_c);
    }

    /* ---- #B003 is write-only, so it reads as the open bus ----------- */
    {
        atom_t *m = machine();
        bus_write(m, 0xB003, ATOM_CTRL_WORD);
        bus_write(m, 0x0000, 0x5A);        /* put a known value on the bus */
        CHECK(bus_read(m, 0xB003) == 0x5A,
              "the control register is not driven, so #B003 reads the open bus");
    }

    TEST_DONE();
}
