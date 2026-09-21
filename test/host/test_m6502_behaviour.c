/* test_m6502_behaviour.c — the NMOS quirks software actually depends on
 * (design.md §6.1), plus the flag arithmetic the Dormann suite would
 * otherwise be the first thing to notice.
 *
 * The read-modify-write double write is not asserted here: with a bare
 * RAM machine both writes land on the same byte and are indistinguishable.
 * It becomes observable at M2, where an RMW on an 8255 register is the
 * case that matters, and the assertion belongs in test_i8255.c.
 */

#include <string.h>

#include "atom.h"
#include "bus.h"
#include "m6502.h"
#include "test_util.h"

static atom_t g_machine;

static atom_t *bare_machine(void) {
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&g_machine, &cfg);
    atom_map_ram(&g_machine, 0x0000, ATOM_ADDR_SPACE);
    memset(g_machine.ram, 0, sizeof(g_machine.ram));

    m6502_t *c = &g_machine.cpu;
    m6502_init(c);
    c->s = 0xFF;
    c->p = M6502_U | M6502_I;
    c->pc = 0x1000;
    return &g_machine;
}

int main(void) {
    /* ---- JMP (xxFF): the high byte comes from the same page --------- */
    {
        atom_t *m = bare_machine();
        m->ram[0x1000] = 0x6C; m->ram[0x1001] = 0xFF; m->ram[0x1002] = 0x30;
        m->ram[0x30FF] = 0x34;   /* low byte  */
        m->ram[0x3100] = 0x12;   /* NOT the high byte on an NMOS part */
        m->ram[0x3000] = 0xAB;   /* this is   */
        m6502_step(m);
        CHECK(m->cpu.pc == 0xAB34, "JMP ($30FF) should wrap to #AB34, got #%04X", m->cpu.pc);
    }

    /* ---- BRK vs IRQ: the flag-B distinction ------------------------- */
    {
        atom_t *m = bare_machine();
        m->ram[0xFFFE] = 0x00; m->ram[0xFFFF] = 0x40;
        m->ram[0x1000] = 0x00;  /* BRK */
        m->ram[0x1001] = 0xEE;  /* signature byte, skipped */
        m->cpu.p = M6502_U;     /* I clear, so we can see BRK set it */
        m6502_step(m);

        CHECK(m->cpu.pc == 0x4000, "BRK should vector to #4000, got #%04X", m->cpu.pc);
        CHECK((m->cpu.p & M6502_I) != 0, "BRK should set I");
        uint8_t pushed_p  = m->ram[0x01FD];
        uint8_t pushed_lo = m->ram[0x01FE];
        uint8_t pushed_hi = m->ram[0x01FF];
        CHECK((pushed_p & M6502_B) != 0, "BRK should push P with B set, got 0x%02X", pushed_p);
        CHECK((pushed_p & M6502_U) != 0, "BRK should push P with the unused bit set");
        CHECK(((pushed_hi << 8) | pushed_lo) == 0x1002,
              "BRK should push the address after the signature byte, got #%04X",
              (pushed_hi << 8) | pushed_lo);
        CHECK(m->cpu.s == 0xFC, "BRK should push three bytes, S=0x%02X", m->cpu.s);
    }
    {
        atom_t *m = bare_machine();
        m->ram[0xFFFE] = 0x00; m->ram[0xFFFF] = 0x40;
        m->ram[0x1000] = 0xEA;
        m->cpu.p = M6502_U;     /* I clear */
        m6502_set_irq(&m->cpu, M6502_IRQ_VIA, true);
        m6502_step(m);
        CHECK(m->cpu.pc == 0x4000, "IRQ should vector to #4000, got #%04X", m->cpu.pc);
        CHECK((m->ram[0x01FD] & M6502_B) == 0,
              "a hardware IRQ should push P with B clear, got 0x%02X", m->ram[0x01FD]);
        CHECK(((m->ram[0x01FF] << 8) | m->ram[0x01FE]) == 0x1000,
              "IRQ should push the interrupted PC unmodified");
    }

    /* IRQ is level-sensitive: deasserting the source stops it firing. */
    {
        atom_t *m = bare_machine();
        m->ram[0x1000] = 0xEA;
        m->cpu.p = M6502_U;
        m6502_set_irq(&m->cpu, M6502_IRQ_VIA, true);
        m6502_set_irq(&m->cpu, M6502_IRQ_VIA, false);
        m6502_step(m);
        CHECK(m->cpu.pc == 0x1001, "a deasserted IRQ should not fire");
    }

    /* Two sources compose; clearing one leaves the line asserted. */
    {
        atom_t *m = bare_machine();
        m->cpu.p = M6502_U;
        m6502_set_irq(&m->cpu, M6502_IRQ_VIA, true);
        m6502_set_irq(&m->cpu, M6502_IRQ_FDC, true);
        m6502_set_irq(&m->cpu, M6502_IRQ_VIA, false);
        CHECK(m->cpu.irq_lines == M6502_IRQ_FDC, "IRQ sources should compose as a mask");
    }

    /* NMI is edge-triggered and latched: a held-high line fires once. */
    {
        atom_t *m = bare_machine();
        m->ram[0xFFFA] = 0x00; m->ram[0xFFFB] = 0x50;
        m->ram[0x1000] = 0xEA; m->ram[0x5000] = 0xEA;
        m->cpu.p |= M6502_I;        /* NMI ignores I */
        m6502_set_nmi(&m->cpu, true);
        m6502_step(m);
        CHECK(m->cpu.pc == 0x5000, "NMI should vector despite I being set, got #%04X",
              m->cpu.pc);
        m6502_set_nmi(&m->cpu, true);   /* still high: no new edge */
        m6502_step(m);
        CHECK(m->cpu.pc == 0x5001, "a held NMI line should not re-trigger");
    }

    /* ---- RTI, PHP and PLP: B is not a real register bit -------------- */
    {
        atom_t *m = bare_machine();
        m->ram[0x1000] = 0x40;          /* RTI */
        m->ram[0x01FD] = 0xFF;          /* P: every bit set, including B */
        m->ram[0x01FE] = 0x34;
        m->ram[0x01FF] = 0x12;
        m->cpu.s = 0xFC;
        m6502_step(m);
        CHECK(m->cpu.pc == 0x1234, "RTI should return to #1234, got #%04X", m->cpu.pc);
        CHECK((m->cpu.p & M6502_B) == 0, "RTI should not set B in the register");
        CHECK((m->cpu.p & M6502_U) != 0, "the unused bit always reads as one");
    }
    {
        atom_t *m = bare_machine();
        m->ram[0x1000] = 0x08;          /* PHP */
        m->cpu.p = M6502_U | M6502_C;
        m6502_step(m);
        CHECK(m->ram[0x01FF] == (M6502_U | M6502_B | M6502_C),
              "PHP should push B and the unused bit set, got 0x%02X", m->ram[0x01FF]);
    }
    {
        atom_t *m = bare_machine();
        m->ram[0x1000] = 0x28;          /* PLP */
        m->ram[0x0100] = 0x10;          /* only B set */
        m->cpu.s = 0xFF;
        m6502_step(m);
        CHECK(m->cpu.p == M6502_U, "PLP should drop B and force the unused bit, got 0x%02X",
              m->cpu.p);
    }

    /* ---- JSR pushes PC-1; RTS adds it back --------------------------- */
    {
        atom_t *m = bare_machine();
        m->ram[0x1000] = 0x20; m->ram[0x1001] = 0x00; m->ram[0x1002] = 0x20; /* JSR $2000 */
        m->ram[0x2000] = 0x60;                                               /* RTS */
        m6502_step(m);
        CHECK(m->cpu.pc == 0x2000, "JSR should land at #2000, got #%04X", m->cpu.pc);
        CHECK(((m->ram[0x01FF] << 8) | m->ram[0x01FE]) == 0x1002,
              "JSR should push the address of its last operand byte");
        m6502_step(m);
        CHECK(m->cpu.pc == 0x1003, "RTS should resume at #1003, got #%04X", m->cpu.pc);
        CHECK(m->cpu.s == 0xFF, "the stack should be balanced after JSR/RTS");
    }

    /* The stack wraps inside page one and never leaves it. */
    {
        atom_t *m = bare_machine();
        m->cpu.s = 0x00;
        m->ram[0x1000] = 0x48;          /* PHA */
        m->cpu.a = 0x5A;
        m6502_step(m);
        CHECK(m->ram[0x0100] == 0x5A, "PHA at S=0 should write #0100");
        CHECK(m->cpu.s == 0xFF, "the stack pointer should wrap to 0xFF, got 0x%02X", m->cpu.s);
    }

    /* ---- ADC / SBC overflow, the four interesting corners ------------ */
    {
        struct { uint8_t a, v, cin, res, c, v_flag; } cases[] = {
            {0x50, 0x10, 0, 0x60, 0, 0},   /*  80 +  16 =  96            */
            {0x50, 0x50, 0, 0xA0, 0, 1},   /*  80 +  80 = -96, overflow  */
            {0xD0, 0x90, 0, 0x60, 1, 1},   /* -48 + -112 = 96, overflow  */
            {0xD0, 0x10, 0, 0xE0, 0, 0},   /* -48 +  16 = -32            */
            {0xFF, 0x01, 0, 0x00, 1, 0},   /* carry out, no overflow     */
            {0x7F, 0x00, 1, 0x80, 0, 1},   /* carry in tips the sign     */
        };
        for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
            atom_t *m = bare_machine();
            m->ram[0x1000] = 0x69; m->ram[0x1001] = cases[i].v;   /* ADC # */
            m->cpu.a = cases[i].a;
            if (cases[i].cin) m->cpu.p |= M6502_C;
            m6502_step(m);
            CHECK(m->cpu.a == cases[i].res, "ADC #$%02X to 0x%02X gave 0x%02X, expected 0x%02X",
                  cases[i].v, cases[i].a, m->cpu.a, cases[i].res);
            CHECK(((m->cpu.p & M6502_C) != 0) == (cases[i].c != 0),
                  "ADC 0x%02X+0x%02X carry wrong", cases[i].a, cases[i].v);
            CHECK(((m->cpu.p & M6502_V) != 0) == (cases[i].v_flag != 0),
                  "ADC 0x%02X+0x%02X overflow wrong", cases[i].a, cases[i].v);
        }
    }

    /* ---- CMP sets C on greater-or-equal and never touches V ---------- */
    {
        atom_t *m = bare_machine();
        m->ram[0x1000] = 0xC9; m->ram[0x1001] = 0x40;   /* CMP #$40 */
        m->cpu.a = 0x40;
        m->cpu.p |= M6502_V;
        m6502_step(m);
        CHECK((m->cpu.p & M6502_C) && (m->cpu.p & M6502_Z), "CMP of equal values sets C and Z");
        CHECK((m->cpu.p & M6502_V) != 0, "CMP must not disturb V");
    }

    /* ---- BIT takes N and V from memory, Z from the AND --------------- */
    {
        atom_t *m = bare_machine();
        m->ram[0x1000] = 0x24; m->ram[0x1001] = 0x20;   /* BIT $20 */
        m->ram[0x0020] = 0xC0;
        m->cpu.a = 0x01;
        m6502_step(m);
        CHECK((m->cpu.p & M6502_N) && (m->cpu.p & M6502_V),
              "BIT should copy bits 7 and 6 of the operand into N and V");
        CHECK((m->cpu.p & M6502_Z) != 0, "BIT should set Z when A & operand is zero");
    }

    /* ---- undocumented opcodes are trapped and logged ----------------- */
    {
        atom_t *m = bare_machine();
        m->ram[0x1000] = 0x02;          /* one of the NMOS jam opcodes */
        uint32_t n = m6502_step(m);
        CHECK(m->cpu.undoc_count == 1, "an undocumented opcode should be counted");
        CHECK(m->cpu.undoc_op == 0x02, "the trapped opcode should be recorded");
        CHECK(m->cpu.undoc_pc == 0x1000, "the trapped PC should be recorded, got #%04X",
              m->cpu.undoc_pc);
        CHECK(n == 2 && m->cpu.pc == 0x1001,
              "a trapped opcode should not wedge the interpreter");
    }

    /* ---- atom_run finishes whole instructions and reports the true
     *      count, which the caller carries as debt (§6.2) ------------- */
    {
        atom_t *m = bare_machine();
        for (int i = 0; i < 64; i++) m->ram[0x1000 + i] = 0xEA;   /* NOP */
        uint32_t ran = atom_run(m, 9);
        CHECK(ran == 10, "five NOPs overshoot a nine-cycle budget by one, got %u", ran);
        CHECK(m->cpu.pc == 0x1005, "five NOPs should advance PC by five, got #%04X",
              m->cpu.pc);
    }

    TEST_DONE();
}
