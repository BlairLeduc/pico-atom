/* test_via6522.c — the 6522 at #B800 (via6522.h).
 *
 * The register semantics are checked against the chip directly; the
 * interrupt path is checked by executing a guest that takes T1 IRQs,
 * because the line from IFR to the 6502 is the part a table cannot see.
 */

#include <string.h>

#include "atom.h"
#include "bus.h"
#include "snapshot.h"
#include "test_util.h"
#include "via6522.h"

static atom_t g_machine, g_other;

static struct { uint8_t b[SNAP_FILE_LEN]; size_t pos; } snap;
static bool mem_write(void *c, const uint8_t *src, size_t n) {
    (void)c; memcpy(snap.b + snap.pos, src, n); snap.pos += n; return true;
}
static bool mem_read(void *c, uint8_t *dst, size_t n) {
    (void)c; memcpy(dst, snap.b + snap.pos, n); snap.pos += n; return true;
}

static void fresh(via6522_t *v) { via6522_reset(v); }

/* Tick a cycle at a time, collecting CB2 at each falling edge of CB1:
 * what a device clocked by CB1 would see shifted out. */
static unsigned shift_out_bits(via6522_t *v, unsigned cycles, uint8_t *got) {
    unsigned n = 0;
    for (unsigned c = 0; c < cycles; c++) {
        bool was = v->cb1;
        via6522_tick(v, 1);
        if (was && !v->cb1) *got = (uint8_t)((*got << 1) | (v->cb2 ? 1u : 0u)), n++;
    }
    return n;
}

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

    /* ---- T2 and the shift clock between their events ------------------ */
    {
        /* The tick leaves them behind (via6522.h); a read catches up. */
        via6522_t v; fresh(&v);
        via6522_write(&v, VIA_T2CL, 0xE8);
        via6522_write(&v, VIA_T2CH, 0x03);                 /* 1000 */
        for (unsigned c = 0; c < 300; c++) via6522_tick(&v, 1);
        CHECK(via6522_read(&v, VIA_T2CL) == 0xBC && via6522_read(&v, VIA_T2CH) == 0x02,
              "T2 reads 700 after 300 cycles a tick at a time");
        for (unsigned c = 0; c < 700; c++) via6522_tick(&v, 1);
        CHECK(!(v.ifr & VIA_INT_T2), "not at zero");
        via6522_tick(&v, 1);
        CHECK(v.ifr & VIA_INT_T2, "flags on the cycle past zero");
        via6522_tick(&v, 3 * 0x10000 + 5);
        CHECK(via6522_read(&v, VIA_T2CL) == 0xFA && via6522_read(&v, VIA_T2CH) == 0xFF,
              "and counts on through its wraps: #%02X%02X",
              via6522_read(&v, VIA_T2CH), via6522_read(&v, VIA_T2CL));

        /* Mode 5 at T2 latch 3 is a half-cycle every 5. A new latch
         * written mid-byte takes effect from the next edge: the fifth, at
         * 25, then eleven more of 10 put the flag at 135. */
        fresh(&v);
        via6522_write(&v, VIA_T2CL, 3);
        via6522_write(&v, VIA_ACR, VIA_SR_OUT_T2 << VIA_ACR_SR_SHIFT);
        via6522_write(&v, VIA_SR, 0x55);
        for (unsigned c = 0; c < 22; c++) via6522_tick(&v, 1);
        via6522_write(&v, VIA_T2CL, 8);
        for (unsigned c = 22; c < 134; c++) via6522_tick(&v, 1);
        CHECK(!(v.ifr & VIA_INT_SR), "not done at 134");
        via6522_tick(&v, 1);
        CHECK(v.ifr & VIA_INT_SR, "done at 135");
    }

    /* ---- T1 on PB7 ---------------------------------------------------- */
    {
        via6522_t v; fresh(&v);
        via6522_write(&v, VIA_ACR, VIA_ACR_T1_PB7);
        via6522_write(&v, VIA_T1CL, 10);
        via6522_write(&v, VIA_T1CH, 0);
        CHECK(!(via6522_pb_out(&v) & 0x80u), "a one-shot takes PB7 low");
        CHECK(!(via6522_read(&v, VIA_ORB) & 0x80u), "and ORB reads it, whatever DDRB says");
        via6522_tick(&v, 10);
        CHECK(!(via6522_pb_out(&v) & 0x80u), "low until T1 runs out");
        via6522_tick(&v, 1);
        CHECK(via6522_pb_out(&v) & 0x80u, "then high");
        via6522_tick(&v, 70000);
        CHECK(via6522_pb_out(&v) & 0x80u, "and stays high");

        via6522_write(&v, VIA_ACR, VIA_ACR_T1_PB7 | VIA_ACR_T1_FREERUN);
        via6522_write(&v, VIA_T1CL, 8);
        via6522_write(&v, VIA_T1CH, 0);
        unsigned edges = 0;
        bool was = false;
        for (unsigned c = 0; c < 100u; c++) {
            via6522_tick(&v, 1);
            bool now = (via6522_pb_out(&v) & 0x80u) != 0;
            if (now != was) edges++;
            was = now;
        }
        CHECK(edges == 10, "free-run at latch 8 toggles PB7 every 10 cycles: %u in 100", edges);
        via6522_write(&v, VIA_ACR, 0);
        via6522_write(&v, VIA_DDRB, 0x80);
        CHECK(!(via6522_pb_out(&v) & 0x80u), "without ACR bit 7, PB7 is ORB's again");
    }

    /* ---- T2 counts pulses on PB6 -------------------------------------- */
    {
        via6522_t v; fresh(&v);
        via6522_write(&v, VIA_ACR, VIA_ACR_T2_PULSES);
        via6522_write(&v, VIA_T2CL, 3);
        via6522_write(&v, VIA_T2CH, 0);
        via6522_tick(&v, 1000);
        CHECK(v.t2 == 3, "counting pulses, cycles pass T2 by: %d", (int)v.t2);
        for (unsigned i = 0; i < 2; i++) { via6522_set_pb(&v, 0xBF); via6522_set_pb(&v, 0xFF); }
        CHECK(!(v.ifr & VIA_INT_T2), "two pulses of three");
        via6522_set_pb(&v, 0xFF);
        CHECK(!(v.ifr & VIA_INT_T2), "a rising edge is not a pulse");
        via6522_set_pb(&v, 0xBF);
        CHECK(v.ifr & VIA_INT_T2, "the third falling edge flags T2");
        CHECK(via6522_read(&v, VIA_T2CL) == 0 && !(v.ifr & VIA_INT_T2), "at zero; read clears");
        via6522_set_pb(&v, 0xFF); via6522_set_pb(&v, 0xBF);
        CHECK(v.t2 == 0xFFFF && !(v.ifr & VIA_INT_T2), "counts on past zero, once only");
    }

    /* ---- CA1: edges, and port A's input latch -------------------------- */
    {
        via6522_t v; fresh(&v);
        via6522_set_ca1(&v, false);
        CHECK(v.ifr & VIA_INT_CA1, "PCR bit 0 clear: CA1 flags a falling edge");
        (void)via6522_read(&v, VIA_ORA_NH);
        CHECK(v.ifr & VIA_INT_CA1, "#B80F leaves the flag");
        (void)via6522_read(&v, VIA_ORA);
        CHECK(!(v.ifr & VIA_INT_CA1), "reading ORA clears it");
        via6522_write(&v, VIA_PCR, 0x01);
        via6522_set_ca1(&v, true);
        CHECK(v.ifr & VIA_INT_CA1, "PCR bit 0 set: a rising edge");
        via6522_write(&v, VIA_ORA, 0);
        CHECK(!(v.ifr & VIA_INT_CA1), "writing ORA clears it too");

        via6522_write(&v, VIA_ACR, VIA_ACR_PA_LATCH);
        via6522_set_pa(&v, 0x12);
        via6522_set_ca1(&v, false);
        via6522_set_ca1(&v, true);
        via6522_set_pa(&v, 0x34);
        CHECK(via6522_read(&v, VIA_ORA) == 0x12, "latched: the pins at the edge");
        via6522_write(&v, VIA_ACR, 0);
        CHECK(via6522_read(&v, VIA_ORA) == 0x34, "unlatched: the pins now");
    }

    /* ---- CA2 in each PCR mode ------------------------------------------ */
    {
        via6522_t v; fresh(&v);
        via6522_set_ca2(&v, false);
        CHECK(v.ifr & VIA_INT_CA2, "mode 0: a falling edge");
        (void)via6522_read(&v, VIA_ORA);
        CHECK(!(v.ifr & VIA_INT_CA2), "cleared by a port A read");

        via6522_write(&v, VIA_PCR, 0x06);                 /* 3: independent, rising */
        via6522_set_ca2(&v, true);
        CHECK(v.ifr & VIA_INT_CA2, "mode 3: a rising edge");
        (void)via6522_read(&v, VIA_ORA);
        CHECK(v.ifr & VIA_INT_CA2, "independent: a port A read leaves it");
        via6522_write(&v, VIA_IFR, VIA_INT_CA2);
        CHECK(!(v.ifr & VIA_INT_CA2), "IFR clears it");

        via6522_write(&v, VIA_PCR, 0x08);                 /* 4: handshake */
        CHECK(v.ca2, "a handshake output idles high");
        (void)via6522_read(&v, VIA_ORA_NH);
        CHECK(v.ca2, "#B80F does not handshake");
        (void)via6522_read(&v, VIA_ORA);
        CHECK(!v.ca2, "a port A read takes it low (data taken)");
        via6522_set_ca2(&v, true);
        CHECK(!v.ca2, "an output ignores the pin");
        via6522_set_ca1(&v, false);
        CHECK(v.ca2, "CA1's active edge ends the handshake");
        via6522_set_ca1(&v, true);
        via6522_write(&v, VIA_ORA, 0x55);
        CHECK(!v.ca2, "so does a port A write, the printer's strobe");

        via6522_write(&v, VIA_PCR, 0x0A);                 /* 5: pulse */
        CHECK(v.ca2 && v.ca2_pulses == 0, "a pulse output idles high");
        via6522_write(&v, VIA_ORA, 0x55);
        (void)via6522_read(&v, VIA_ORA);
        via6522_write(&v, VIA_ORA_NH, 0x55);
        CHECK(v.ca2 && v.ca2_pulses == 2, "one pulse per ORA access: %u", v.ca2_pulses);

        via6522_write(&v, VIA_PCR, 0x0C);
        CHECK(!v.ca2, "mode 6 holds CA2 low");
        via6522_write(&v, VIA_PCR, 0x0E);
        CHECK(v.ca2, "mode 7 holds it high");
    }

    /* ---- CB1, CB2 and port B ---------------------------------------------- */
    {
        via6522_t v; fresh(&v);
        via6522_write(&v, VIA_PCR, 0x80);                 /* CB2 handshake */
        CHECK(v.cb2, "CB2 handshake idles high");
        (void)via6522_read(&v, VIA_ORB);
        CHECK(v.cb2, "reading port B does not handshake");
        via6522_write(&v, VIA_ORB, 1);
        CHECK(!v.cb2, "writing it does");
        via6522_set_cb1(&v, false);
        CHECK(v.cb2 && (v.ifr & VIA_INT_CB1), "CB1's edge ends it and flags");
        via6522_write(&v, VIA_ORB, 1);
        CHECK(!(v.ifr & VIA_INT_CB1), "a port B access clears CB1");

        via6522_write(&v, VIA_PCR, 0x00);
        via6522_set_cb2(&v, true);             /* the handshake left the pin low */
        CHECK(!(v.ifr & VIA_INT_CB2), "mode 0 ignores a rising edge");
        via6522_set_cb2(&v, false);
        CHECK(v.ifr & VIA_INT_CB2, "CB2 input flags a falling edge");
        (void)via6522_read(&v, VIA_ORB);
        CHECK(!(v.ifr & VIA_INT_CB2), "cleared by a port B read");

        via6522_write(&v, VIA_ACR, VIA_ACR_PB_LATCH);
        via6522_write(&v, VIA_DDRB, 0xF0);
        via6522_write(&v, VIA_ORB, 0xA0);
        via6522_set_pb(&v, 0x05);
        via6522_set_cb1(&v, true);                         /* PCR bit 4 clear: not active */
        via6522_set_cb1(&v, false);
        via6522_set_pb(&v, 0x0A);
        CHECK(via6522_read(&v, VIA_ORB) == 0xA5, "PB latched on CB1: 0x%02X",
              via6522_read(&v, VIA_ORB));
    }

    /* ---- the shift register --------------------------------------------- */
    {
        via6522_t v; fresh(&v);
        uint8_t got = 0;

        /* Mode 6, out under Φ2: a bit every two cycles. */
        via6522_write(&v, VIA_ACR, VIA_SR_OUT_PHI2 << VIA_ACR_SR_SHIFT);
        via6522_write(&v, VIA_SR, 0xA5);
        unsigned n = shift_out_bits(&v, 15, &got);
        CHECK(!(v.ifr & VIA_INT_SR), "not done after 15 cycles");
        n += shift_out_bits(&v, 1, &got);
        CHECK(v.ifr & VIA_INT_SR, "eight bits in 16 cycles flag SR");
        CHECK(n == 8 && got == 0xA5, "shifted out MSB first: %u bits, 0x%02X", n, got);
        CHECK(v.sr == 0xA5 && v.cb1, "the byte comes round; CB1 rests high");
        n = shift_out_bits(&v, 100, &got);
        CHECK(n == 0, "and stops");
        (void)via6522_read(&v, VIA_SR);
        CHECK(!(v.ifr & VIA_INT_SR), "reading SR clears the flag");

        /* Mode 5, out under T2: half-cycles of latch + 2. */
        fresh(&v);
        via6522_write(&v, VIA_T2CL, 3);
        via6522_write(&v, VIA_ACR, VIA_SR_OUT_T2 << VIA_ACR_SR_SHIFT);
        via6522_write(&v, VIA_SR, 0x3C);
        got = 0;
        n = shift_out_bits(&v, 79, &got);
        CHECK(!(v.ifr & VIA_INT_SR), "T2 at 3: not done after 79 cycles");
        n += shift_out_bits(&v, 1, &got);
        CHECK((v.ifr & VIA_INT_SR) && n == 8 && got == 0x3C, "done at 80: 0x%02X", got);

        /* Mode 1, in under T2: CB2 sampled on each rising CB1. */
        fresh(&v);
        via6522_write(&v, VIA_T2CL, 0);
        via6522_write(&v, VIA_ACR, VIA_SR_IN_T2 << VIA_ACR_SR_SHIFT);
        (void)via6522_read(&v, VIA_SR);
        const uint8_t in = 0x96;
        unsigned bit = 0;
        for (unsigned c = 0; c < 64 && !(v.ifr & VIA_INT_SR); c++) {
            bool was = v.cb1;
            via6522_tick(&v, 1);
            if (was && !v.cb1) via6522_set_cb2(&v, (in >> (7u - bit++)) & 1u);
        }
        CHECK((v.ifr & VIA_INT_SR) && v.sr == in, "shifted in 0x%02X", v.sr);
        CHECK(!(v.ifr & VIA_INT_CB2), "CB2 as shift data raises no CB2 flag");

        /* Mode 3 and 7: a clock from outside. */
        fresh(&v);
        via6522_write(&v, VIA_ACR, VIA_SR_IN_EXT << VIA_ACR_SR_SHIFT);
        (void)via6522_read(&v, VIA_SR);
        for (int b = 7; b >= 0; b--) {
            via6522_set_cb2(&v, (0x5Au >> b) & 1u);
            via6522_set_cb1(&v, false);
            via6522_set_cb1(&v, true);
        }
        CHECK((v.ifr & VIA_INT_SR) && v.sr == 0x5A, "clocked in from CB1: 0x%02X", v.sr);

        fresh(&v);
        via6522_write(&v, VIA_ACR, VIA_SR_OUT_EXT << VIA_ACR_SR_SHIFT);
        via6522_write(&v, VIA_SR, 0x81);
        got = 0;
        for (unsigned b = 0; b < 8; b++) {
            via6522_set_cb1(&v, false);
            got = (uint8_t)((got << 1) | (v.cb2 ? 1u : 0u));
            via6522_set_cb1(&v, true);
        }
        CHECK((v.ifr & VIA_INT_SR) && got == 0x81, "clocked out by CB1: 0x%02X", got);
        via6522_set_cb2(&v, !v.cb2);
        CHECK(v.cb2 == ((0x81u & 1u) != 0), "CB2 is the shift register's while it shifts out");

        /* Mode 4 runs for ever without a flag; mode 0 stops it. */
        fresh(&v);
        via6522_write(&v, VIA_T2CL, 0);
        via6522_write(&v, VIA_ACR, VIA_SR_OUT_FREE << VIA_ACR_SR_SHIFT);
        via6522_write(&v, VIA_SR, 0x0F);
        got = 0;
        n = shift_out_bits(&v, 320, &got);
        CHECK(n == 80 && !(v.ifr & VIA_INT_SR) && v.sr == 0x0F && got == 0x0F,
              "free-running: %u bits, no flag, the byte recirculating", n);
        via6522_write(&v, VIA_ACR, 0);
        CHECK(shift_out_bits(&v, 100, &got) == 0, "off stops it");

        /* A tape stall passes a slice in one tick (§11.2). */
        fresh(&v);
        via6522_write(&v, VIA_ACR, VIA_SR_OUT_PHI2 << VIA_ACR_SR_SHIFT);
        via6522_write(&v, VIA_SR, 1);
        via6522_tick(&v, 40000);
        CHECK((v.ifr & VIA_INT_SR) && v.sr == 1 && v.cb1, "a long tick finishes the byte");
    }

    /* ---- the rest of the part survives a snapshot ------------------------ */
    {
        atom_t *m = machine();
        via6522_t *v = &m->via;
        bus_write(m, 0xB80B, (VIA_SR_OUT_T2 << VIA_ACR_SR_SHIFT) | VIA_ACR_T1_PB7 | VIA_ACR_PA_LATCH);
        bus_write(m, 0xB80C, 0x0C);                        /* CA2 low */
        bus_write(m, 0xB808, 20);
        via6522_set_pa(v, 0x42);
        via6522_set_ca1(v, false);
        bus_write(m, 0xB805, 0x40);                        /* PB7 low */
        bus_write(m, 0xB80A, 0xC3);
        via6522_tick(v, 50);                               /* mid-byte */
        via6522_t before = *v;
        via6522_sync(&before);
        snap.pos = 0;
        CHECK(snapshot_save(m, mem_write, NULL) == SNAP_OK, "save");

        atom_config_t cfg;
        atom_config_default(&cfg);
        atom_init(&g_other, &cfg);
        snap.pos = 0;
        CHECK(snapshot_load(&g_other, mem_read, NULL) == SNAP_OK, "load");
        const via6522_t *w = &g_other.via;
        CHECK(w->ira == 0x42 && !w->pb7 && !w->ca1 && !w->ca2 && w->cb1 == before.cb1 &&
              w->cb2 == before.cb2 && w->sr == before.sr && w->sr_halves == before.sr_halves &&
              w->sr_timer == before.sr_timer, "latch, lines and shift state come back");
        via6522_tick(&g_other.via, 1000);
        via6522_tick(v, 1000);
        CHECK(w->sr == v->sr && (w->ifr & VIA_INT_SR) && (v->ifr & VIA_INT_SR),
              "and the byte finishes the same");
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
