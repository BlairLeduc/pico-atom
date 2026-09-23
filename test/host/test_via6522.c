/* test_via6522.c — the 6522 at #B800 (via6522.h).
 *
 * The register semantics are checked against the chip directly; the
 * interrupt path is checked by executing a guest that takes T1 IRQs,
 * because the line from IFR to the 6502 is the part a table cannot see.
 */

#include "atom.h"
#include "bus.h"
#include "test_util.h"
#include "via6522.h"

static atom_t g_machine;

static atom_t *machine(void) {
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&g_machine, &cfg);
    return &g_machine;
}

int main(void) {
    /* ---- fitted by default, and what the MOS reads first ------------- */
    {
        atom_t *m = machine();
        CHECK(m->cfg.via_fitted, "the MOS needs the VIA (via6522.h)");

        /* The MOS's printer test at #FF0A: PCR & #0E must read zero on a
         * fresh machine, or it spins on #B801 before the first prompt. */
        bus_write(m, 0x0000, 0xB8);                 /* poison the open bus */
        CHECK((bus_read(m, 0xB80C) & 0x0Eu) == 0, "PCR should read 0 at power-on");

        /* With DDRA = #7F, as the MOS sets it, bit 7 is BUSY: no printer
         * is attached, so it reads ready. */
        bus_write(m, 0xB803, 0x7F);
        CHECK((bus_read(m, 0xB801) & 0x80u) == 0, "no printer should read ready");

        /* The VIA decodes by mask through #B800-#BBFF (§7.3). */
        bus_write(m, 0xB80B, 0x5A);
        CHECK(bus_read(m, 0xB81B) == 0x5A, "#B81B mirrors the ACR");
    }

    /* ---- ports: output bits read back, input bits read the pins ------ */
    {
        via6522_t v;
        via6522_reset(&v);
        v.in_b = 0x0F;
        via6522_write(&v, VIA_DDRB, 0xF0);
        via6522_write(&v, VIA_ORB, 0xA5);
        CHECK(via6522_read(&v, VIA_ORB) == 0xAF, "ORB mixes latch and pins, got 0x%02X",
              via6522_read(&v, VIA_ORB));
    }

    /* ---- T1 one-shot: one flag, then silence ------------------------- */
    {
        via6522_t v;
        via6522_reset(&v);
        via6522_write(&v, VIA_IER, 0x80u | VIA_INT_T1);
        via6522_write(&v, VIA_T1CL, 100);
        via6522_write(&v, VIA_T1CH, 0);
        via6522_tick(&v, 100);
        CHECK(!(v.ifr & VIA_INT_T1), "T1 at 0 has not underflowed yet");
        via6522_tick(&v, 1);
        CHECK(v.ifr & VIA_INT_T1, "T1 flags on underflow");
        CHECK(via6522_irq(&v), "and asserts IRQ when enabled");
        CHECK(via6522_read(&v, VIA_IFR) == (0x80u | VIA_INT_T1), "IFR bit 7 follows");

        (void)via6522_read(&v, VIA_T1CL);           /* reading T1C-L clears it */
        CHECK(!(v.ifr & VIA_INT_T1) && !via6522_irq(&v), "T1C-L read clears T1");
        via6522_tick(&v, 70000);
        CHECK(!(v.ifr & VIA_INT_T1), "one-shot must not fire again unarmed");
    }

    /* ---- T1 free-run: period latch + 2 ------------------------------- */
    {
        via6522_t v;
        via6522_reset(&v);
        via6522_write(&v, VIA_ACR, VIA_ACR_T1_FREERUN);
        via6522_write(&v, VIA_T1CL, 98);
        via6522_write(&v, VIA_T1CH, 0);
        unsigned fired = 0;
        for (unsigned c = 0; c < 100u * 50u + 98u; c++) {
            via6522_tick(&v, 1);
            if (v.ifr & VIA_INT_T1) { fired++; via6522_write(&v, VIA_IFR, VIA_INT_T1); }
        }
        CHECK(fired == 50, "free-run at latch 98 fired %u times in 5098 cycles", fired);
    }

    /* ---- T2 one-shot, and IER write semantics ------------------------ */
    {
        via6522_t v;
        via6522_reset(&v);
        via6522_write(&v, VIA_T2CL, 10);
        via6522_write(&v, VIA_T2CH, 0);
        via6522_tick(&v, 11);
        CHECK(v.ifr & VIA_INT_T2, "T2 flags on underflow");
        CHECK(!via6522_irq(&v), "but IRQ stays low while T2 is disabled");

        via6522_write(&v, VIA_IER, 0x80u | VIA_INT_T2 | VIA_INT_T1);
        CHECK(via6522_read(&v, VIA_IER) == (0x80u | VIA_INT_T2 | VIA_INT_T1), "IER set");
        via6522_write(&v, VIA_IER, VIA_INT_T1);          /* bit 7 clear: clear */
        CHECK(via6522_read(&v, VIA_IER) == (0x80u | VIA_INT_T2), "IER clear");
        CHECK(via6522_irq(&v), "enabled T2 flag asserts IRQ");
        via6522_write(&v, VIA_IFR, 0xFF);
        CHECK(v.ifr == 0 && !via6522_irq(&v), "writing IFR clears flags");
    }

    /* ---- executed: a free-running T1 interrupts the 6502 ------------- *
     *
     * #1000  LDA #40 ; STA #B80B   ACR: T1 free-run
     *        LDA #C0 ; STA #B80E   IER: enable T1
     *        LDA #E6 ; STA #B804   latch #03E6 = 998 -> period 1000
     *        LDA #03 ; STA #B805   start
     *        CLI
     * #1015  JMP #1015
     * #1100  (IRQ) LDA #B804 ; INC #70 ; RTI     reading T1C-L acks it */
    {
        atom_t *m = machine();
        static const uint8_t main_prog[] = {
            0xA9, 0x40, 0x8D, 0x0B, 0xB8,
            0xA9, 0xC0, 0x8D, 0x0E, 0xB8,
            0xA9, 0xE6, 0x8D, 0x04, 0xB8,
            0xA9, 0x03, 0x8D, 0x05, 0xB8,
            0x58,
            0x4C, 0x15, 0x10,
        };
        static const uint8_t isr[] = { 0xAD, 0x04, 0xB8, 0xE6, 0x70, 0x40 };
        for (unsigned i = 0; i < sizeof main_prog; i++)
            bus_write(m, (uint16_t)(0x1000u + i), main_prog[i]);
        for (unsigned i = 0; i < sizeof isr; i++)
            bus_write(m, (uint16_t)(0x1100u + i), isr[i]);
        atom_map_ram(m, 0xFF00, 0x100);
        bus_write(m, 0xFFFE, 0x00);
        bus_write(m, 0xFFFF, 0x11);
        m->cpu.pc = 0x1000;

        atom_run(m, 100000);
        CHECK(m->ram[0x70] >= 99 && m->ram[0x70] <= 100,
              "100000 cycles at a 1000-cycle period took %u IRQs", m->ram[0x70]);
    }

    /* ---- not fitted: the page is open bus, and nothing interrupts ---- */
    {
        atom_config_t cfg;
        atom_config_default(&cfg);
        cfg.via_fitted = false;
        atom_init(&g_machine, &cfg);
        bus_write(&g_machine, 0x0000, 0x5A);
        CHECK(bus_read(&g_machine, 0xB80E) == 0x5A, "no VIA: #B80E is open bus");
    }

    TEST_DONE();
}
