/* m6502.c — MOS 6502 interpreter (design.md §6).
 *
 * Switch-dispatched with explicit cycle accounting, not a jump table of
 * function pointers: on Cortex-M33 a dense switch compiles to a
 * table-indexed branch with no call overhead (§6.2).
 *
 * Accuracy target (§6.1):
 *   - all 151 documented opcodes with exact cycle counts, including the
 *     extra cycle on page-crossing indexed reads and on taken branches,
 *     and a further cycle when a branch crosses a page;
 *   - decimal mode exact, including NMOS flag behaviour;
 *   - the JMP (xxFF) page wrap, the BRK/IRQ flag-B distinction and the
 *     read-modify-write double write;
 *   - undocumented opcodes trap and log.
 */

#include "m6502.h"

#include "atom.h"
#include "bus.h"
#include "hot.h"

/* Base cycles per opcode, before page-cross and branch penalties.
 * Zero marks an undocumented opcode (§6.1: v1 traps and logs them). */
static const uint8_t base_cycles[256] = {
/*      x0 x1 x2 x3 x4 x5 x6 x7 x8 x9 xA xB xC xD xE xF */
/*0x*/   7, 6, 0, 0, 0, 3, 5, 0, 3, 2, 2, 0, 0, 4, 6, 0,
/*1x*/   2, 5, 0, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
/*2x*/   6, 6, 0, 0, 3, 3, 5, 0, 4, 2, 2, 0, 4, 4, 6, 0,
/*3x*/   2, 5, 0, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
/*4x*/   6, 6, 0, 0, 0, 3, 5, 0, 3, 2, 2, 0, 3, 4, 6, 0,
/*5x*/   2, 5, 0, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
/*6x*/   6, 6, 0, 0, 0, 3, 5, 0, 4, 2, 2, 0, 5, 4, 6, 0,
/*7x*/   2, 5, 0, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
/*8x*/   0, 6, 0, 0, 3, 3, 3, 0, 2, 0, 2, 0, 4, 4, 4, 0,
/*9x*/   2, 6, 0, 0, 4, 4, 4, 0, 2, 5, 2, 0, 0, 5, 0, 0,
/*Ax*/   2, 6, 2, 0, 3, 3, 3, 0, 2, 2, 2, 0, 4, 4, 4, 0,
/*Bx*/   2, 5, 0, 0, 4, 4, 4, 0, 2, 4, 2, 0, 4, 4, 4, 0,
/*Cx*/   2, 6, 0, 0, 3, 3, 5, 0, 2, 2, 2, 0, 4, 4, 6, 0,
/*Dx*/   2, 5, 0, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
/*Ex*/   2, 6, 0, 0, 3, 3, 5, 0, 2, 2, 2, 0, 4, 4, 6, 0,
/*Fx*/   2, 5, 0, 0, 0, 4, 6, 0, 2, 4, 0, 0, 0, 4, 7, 0,
};

uint8_t m6502_base_cycles(uint8_t opcode) { return base_cycles[opcode]; }
bool    m6502_is_documented(uint8_t opcode) { return base_cycles[opcode] != 0; }

void m6502_init(m6502_t *c) {
    c->pc = 0;
    c->a = c->x = c->y = 0;
    c->s = 0xFD;
    c->p = M6502_U | M6502_I;
    c->cycles = 0;
    c->irq_lines = 0;
    c->nmi_pending = false;
    c->nmi_line = false;
    c->reset_pending = false;
    c->undoc_count = 0;
    c->undoc_pc = 0;
    c->undoc_op = 0;
}

void m6502_set_nmi(m6502_t *c, bool level) {
    if (level && !c->nmi_line) c->nmi_pending = true;   /* edge-triggered */
    c->nmi_line = level;
}

void ATOM_HOT1(m6502_set_irq)(m6502_t *c, uint8_t source, bool asserted) {
    if (asserted) c->irq_lines |= source;
    else          c->irq_lines = (uint8_t)(c->irq_lines & ~source);
}

/* ---- inner helpers --------------------------------------------------- */

#define RD(a)    bus_read(m, (uint16_t)(a))
#define WR(a, v) bus_write(m, (uint16_t)(a), (uint8_t)(v))

static inline uint8_t fetch8(atom_t *m, m6502_t *c) {
    return RD(c->pc++);
}

/* GCC keeps this one out of line, so it moves with the interpreter;
 * left behind, every absolute operand calls into flash through a veneer.
 * That measured nothing (§6.3), but the SRAM code then stays whole. */
static inline uint16_t ATOM_HOT2(fetch16)(atom_t *m, m6502_t *c) {
    uint16_t lo = fetch8(m, c);
    uint16_t hi = fetch8(m, c);
    return (uint16_t)(lo | (hi << 8));
}

static inline void push8(atom_t *m, m6502_t *c, uint8_t v) {
    WR(0x0100u | c->s, v);
    c->s--;
}

static inline uint8_t pull8(atom_t *m, m6502_t *c) {
    c->s++;
    return RD(0x0100u | c->s);
}

static inline void set_nz(m6502_t *c, uint8_t v) {
    c->p = (uint8_t)((c->p & ~(M6502_N | M6502_Z)) | (v & M6502_N) |
                     (v == 0 ? M6502_Z : 0));
}

static inline void set_flag(m6502_t *c, uint8_t bit, bool on) {
    if (on) c->p |= bit;
    else    c->p = (uint8_t)(c->p & ~bit);
}

/* Zero-page indirect fetch wraps within page zero. */
static inline uint16_t read16_zp(atom_t *m, uint8_t zp) {
    uint16_t lo = RD(zp);
    uint16_t hi = RD((uint8_t)(zp + 1));
    return (uint16_t)(lo | (hi << 8));
}

/* ---- arithmetic ------------------------------------------------------ */

/* ADC. In decimal mode the NMOS part sets Z from the binary result and N
 * and V from the intermediate value after the low-nibble fixup but before
 * the high-nibble one — the behaviour Bruce Clark's test checks (§6.1). */
static void ATOM_HOT1(op_adc)(m6502_t *c, uint8_t v) {
    unsigned cin = (c->p & M6502_C) ? 1u : 0u;
    unsigned bin = (unsigned)c->a + v + cin;

    if (c->p & M6502_D) {
        unsigned al = (unsigned)(c->a & 0x0Fu) + (v & 0x0Fu) + cin;
        unsigned ah = (unsigned)(c->a >> 4) + (v >> 4);
        if (al > 9u) { al += 6u; ah += 1u; }

        unsigned mid = ((ah << 4) | (al & 0x0Fu)) & 0xFFu;
        set_flag(c, M6502_N, (mid & 0x80u) != 0);
        set_flag(c, M6502_V, ((~((unsigned)c->a ^ v) & ((unsigned)c->a ^ mid)) & 0x80u) != 0);
        set_flag(c, M6502_Z, (bin & 0xFFu) == 0u);

        if (ah > 9u) ah += 6u;
        set_flag(c, M6502_C, ah > 15u);
        c->a = (uint8_t)(((ah << 4) | (al & 0x0Fu)) & 0xFFu);
    } else {
        set_flag(c, M6502_C, bin > 0xFFu);
        set_flag(c, M6502_V, ((~((unsigned)c->a ^ v) & ((unsigned)c->a ^ bin)) & 0x80u) != 0);
        c->a = (uint8_t)bin;
        set_nz(c, c->a);
    }
}

/* SBC. All four flags come from the binary operation on the NMOS part,
 * decimal mode or not; only the result is BCD-adjusted. */
static void ATOM_HOT1(op_sbc)(m6502_t *c, uint8_t v) {
    unsigned cin = (c->p & M6502_C) ? 1u : 0u;
    unsigned bin = (unsigned)c->a - v - (1u - cin);

    set_flag(c, M6502_C, (bin & 0x100u) == 0u);
    set_flag(c, M6502_V, ((((unsigned)c->a ^ v) & ((unsigned)c->a ^ bin)) & 0x80u) != 0);
    set_nz(c, (uint8_t)bin);

    if (c->p & M6502_D) {
        unsigned al = (unsigned)(c->a & 0x0Fu) - (v & 0x0Fu) - (1u - cin);
        unsigned ah = (unsigned)(c->a >> 4) - (v >> 4);
        if (al & 0x10u) { al -= 6u; ah -= 1u; }
        if (ah & 0x10u) { ah -= 6u; }
        c->a = (uint8_t)(((ah << 4) | (al & 0x0Fu)) & 0xFFu);
    } else {
        c->a = (uint8_t)bin;
    }
}

static inline void op_cmp(m6502_t *c, uint8_t reg, uint8_t v) {
    unsigned d = (unsigned)reg - v;
    set_flag(c, M6502_C, reg >= v);
    set_nz(c, (uint8_t)d);
}

static inline uint8_t op_asl(m6502_t *c, uint8_t v) {
    set_flag(c, M6502_C, (v & 0x80u) != 0);
    v = (uint8_t)(v << 1);
    set_nz(c, v);
    return v;
}

static inline uint8_t op_lsr(m6502_t *c, uint8_t v) {
    set_flag(c, M6502_C, (v & 0x01u) != 0);
    v = (uint8_t)(v >> 1);
    set_nz(c, v);
    return v;
}

static inline uint8_t op_rol(m6502_t *c, uint8_t v) {
    uint8_t cin = (uint8_t)((c->p & M6502_C) ? 1u : 0u);
    set_flag(c, M6502_C, (v & 0x80u) != 0);
    v = (uint8_t)((v << 1) | cin);
    set_nz(c, v);
    return v;
}

static inline uint8_t op_ror(m6502_t *c, uint8_t v) {
    uint8_t cin = (uint8_t)((c->p & M6502_C) ? 0x80u : 0u);
    set_flag(c, M6502_C, (v & 0x01u) != 0);
    v = (uint8_t)((v >> 1) | cin);
    set_nz(c, v);
    return v;
}

/* ---- interrupt entry ------------------------------------------------- */

/* BRK pushes P with B set; a hardware IRQ or NMI pushes it clear. That
 * distinction is the only way an interrupt handler can tell them apart. */
static uint32_t ATOM_HOT1(enter_interrupt)(atom_t *m, m6502_t *c, uint16_t vector, bool from_brk) {
    push8(m, c, (uint8_t)(c->pc >> 8));
    push8(m, c, (uint8_t)(c->pc & 0xFFu));
    push8(m, c, (uint8_t)(c->p | M6502_U | (from_brk ? M6502_B : 0u)));
    c->p |= M6502_I;
    uint16_t lo = RD(vector);
    uint16_t hi = RD(vector + 1u);
    c->pc = (uint16_t)(lo | (hi << 8));
    return 7u;
}

void m6502_reset(m6502_t *c, atom_t *m) {
    /* Reset does not clear the registers on real silicon; it decrements
     * the stack pointer three times without writing and sets I. */
    c->s = (uint8_t)(c->s - 3u);
    c->p = (uint8_t)((c->p | M6502_I | M6502_U) & ~M6502_D);
    uint16_t lo = RD(M6502_VEC_RES);
    uint16_t hi = RD(M6502_VEC_RES + 1u);
    c->pc = (uint16_t)(lo | (hi << 8));
    c->nmi_pending = false;
    c->reset_pending = false;
    c->cycles += 7u;
}

/* ---- the interpreter ------------------------------------------------- */

uint32_t ATOM_HOT2(m6502_step)(atom_t *m) {
    m6502_t *c = &m->cpu;

    if (__builtin_expect(c->reset_pending, 0)) {
        uint64_t before = c->cycles;
        m6502_reset(c, m);
        return (uint32_t)(c->cycles - before);
    }
    if (__builtin_expect(c->nmi_pending, 0)) {
        c->nmi_pending = false;
        uint32_t n = enter_interrupt(m, c, M6502_VEC_NMI, false);
        c->cycles += n;
        return n;
    }
    if (__builtin_expect(c->irq_lines != 0 && !(c->p & M6502_I), 0)) {
        uint32_t n = enter_interrupt(m, c, M6502_VEC_IRQ, false);
        c->cycles += n;
        return n;
    }

    uint8_t op = fetch8(m, c);
    uint32_t cyc = base_cycles[op];
    uint16_t ea;
    uint16_t base;
    uint8_t  v;

/* Addressing modes. The _r forms charge the page-crossing cycle; the _w
 * and RMW forms do not, because their base count already includes it. */
#define A_IMM()   (ea = c->pc++)
#define A_ZP()    (ea = fetch8(m, c))
#define A_ZPX()   (ea = (uint8_t)(fetch8(m, c) + c->x))
#define A_ZPY()   (ea = (uint8_t)(fetch8(m, c) + c->y))
#define A_ABS()   (ea = fetch16(m, c))
#define A_ABSX_R() do { base = fetch16(m, c); ea = (uint16_t)(base + c->x); \
                        if ((base ^ ea) & 0xFF00u) cyc++; } while (0)
#define A_ABSY_R() do { base = fetch16(m, c); ea = (uint16_t)(base + c->y); \
                        if ((base ^ ea) & 0xFF00u) cyc++; } while (0)
#define A_ABSX_W() (ea = (uint16_t)(fetch16(m, c) + c->x))
#define A_ABSY_W() (ea = (uint16_t)(fetch16(m, c) + c->y))
#define A_INDX()   (ea = read16_zp(m, (uint8_t)(fetch8(m, c) + c->x)))
#define A_INDY_R() do { base = read16_zp(m, fetch8(m, c)); ea = (uint16_t)(base + c->y); \
                        if ((base ^ ea) & 0xFF00u) cyc++; } while (0)
#define A_INDY_W() (ea = (uint16_t)(read16_zp(m, fetch8(m, c)) + c->y))

/* Read-modify-write writes the unmodified value back before the result —
 * the NMOS double write, which some hardware depends on (§6.1). */
#define RMW(fn) do { v = RD(ea); WR(ea, v); WR(ea, fn(c, v)); } while (0)

#define BRANCH(cond) do {                                                 \
        int8_t off = (int8_t)fetch8(m, c);                                \
        if (cond) {                                                       \
            uint16_t dst = (uint16_t)(c->pc + off);                       \
            cyc++;                                                        \
            if ((dst ^ c->pc) & 0xFF00u) cyc++;                           \
            c->pc = dst;                                                  \
        }                                                                 \
    } while (0)

    switch (op) {
    /* ---- load / store ------------------------------------------------ */
    case 0xA9: A_IMM();    c->a = RD(ea); set_nz(c, c->a); break;
    case 0xA5: A_ZP();     c->a = RD(ea); set_nz(c, c->a); break;
    case 0xB5: A_ZPX();    c->a = RD(ea); set_nz(c, c->a); break;
    case 0xAD: A_ABS();    c->a = RD(ea); set_nz(c, c->a); break;
    case 0xBD: A_ABSX_R(); c->a = RD(ea); set_nz(c, c->a); break;
    case 0xB9: A_ABSY_R(); c->a = RD(ea); set_nz(c, c->a); break;
    case 0xA1: A_INDX();   c->a = RD(ea); set_nz(c, c->a); break;
    case 0xB1: A_INDY_R(); c->a = RD(ea); set_nz(c, c->a); break;

    case 0xA2: A_IMM();    c->x = RD(ea); set_nz(c, c->x); break;
    case 0xA6: A_ZP();     c->x = RD(ea); set_nz(c, c->x); break;
    case 0xB6: A_ZPY();    c->x = RD(ea); set_nz(c, c->x); break;
    case 0xAE: A_ABS();    c->x = RD(ea); set_nz(c, c->x); break;
    case 0xBE: A_ABSY_R(); c->x = RD(ea); set_nz(c, c->x); break;

    case 0xA0: A_IMM();    c->y = RD(ea); set_nz(c, c->y); break;
    case 0xA4: A_ZP();     c->y = RD(ea); set_nz(c, c->y); break;
    case 0xB4: A_ZPX();    c->y = RD(ea); set_nz(c, c->y); break;
    case 0xAC: A_ABS();    c->y = RD(ea); set_nz(c, c->y); break;
    case 0xBC: A_ABSX_R(); c->y = RD(ea); set_nz(c, c->y); break;

    case 0x85: A_ZP();     WR(ea, c->a); break;
    case 0x95: A_ZPX();    WR(ea, c->a); break;
    case 0x8D: A_ABS();    WR(ea, c->a); break;
    case 0x9D: A_ABSX_W(); WR(ea, c->a); break;
    case 0x99: A_ABSY_W(); WR(ea, c->a); break;
    case 0x81: A_INDX();   WR(ea, c->a); break;
    case 0x91: A_INDY_W(); WR(ea, c->a); break;

    case 0x86: A_ZP();     WR(ea, c->x); break;
    case 0x96: A_ZPY();    WR(ea, c->x); break;
    case 0x8E: A_ABS();    WR(ea, c->x); break;

    case 0x84: A_ZP();     WR(ea, c->y); break;
    case 0x94: A_ZPX();    WR(ea, c->y); break;
    case 0x8C: A_ABS();    WR(ea, c->y); break;

    /* ---- transfers --------------------------------------------------- */
    case 0xAA: c->x = c->a; set_nz(c, c->x); break;  /* TAX */
    case 0xA8: c->y = c->a; set_nz(c, c->y); break;  /* TAY */
    case 0xBA: c->x = c->s; set_nz(c, c->x); break;  /* TSX */
    case 0x8A: c->a = c->x; set_nz(c, c->a); break;  /* TXA */
    case 0x9A: c->s = c->x; break;                   /* TXS — no flags */
    case 0x98: c->a = c->y; set_nz(c, c->a); break;  /* TYA */

    /* ---- stack ------------------------------------------------------- */
    case 0x48: push8(m, c, c->a); break;                              /* PHA */
    case 0x08: push8(m, c, (uint8_t)(c->p | M6502_B | M6502_U)); break; /* PHP */
    case 0x68: c->a = pull8(m, c); set_nz(c, c->a); break;            /* PLA */
    case 0x28: c->p = (uint8_t)((pull8(m, c) & ~M6502_B) | M6502_U); break; /* PLP */

    /* ---- logic ------------------------------------------------------- */
    case 0x29: A_IMM();    c->a &= RD(ea); set_nz(c, c->a); break;
    case 0x25: A_ZP();     c->a &= RD(ea); set_nz(c, c->a); break;
    case 0x35: A_ZPX();    c->a &= RD(ea); set_nz(c, c->a); break;
    case 0x2D: A_ABS();    c->a &= RD(ea); set_nz(c, c->a); break;
    case 0x3D: A_ABSX_R(); c->a &= RD(ea); set_nz(c, c->a); break;
    case 0x39: A_ABSY_R(); c->a &= RD(ea); set_nz(c, c->a); break;
    case 0x21: A_INDX();   c->a &= RD(ea); set_nz(c, c->a); break;
    case 0x31: A_INDY_R(); c->a &= RD(ea); set_nz(c, c->a); break;

    case 0x49: A_IMM();    c->a ^= RD(ea); set_nz(c, c->a); break;
    case 0x45: A_ZP();     c->a ^= RD(ea); set_nz(c, c->a); break;
    case 0x55: A_ZPX();    c->a ^= RD(ea); set_nz(c, c->a); break;
    case 0x4D: A_ABS();    c->a ^= RD(ea); set_nz(c, c->a); break;
    case 0x5D: A_ABSX_R(); c->a ^= RD(ea); set_nz(c, c->a); break;
    case 0x59: A_ABSY_R(); c->a ^= RD(ea); set_nz(c, c->a); break;
    case 0x41: A_INDX();   c->a ^= RD(ea); set_nz(c, c->a); break;
    case 0x51: A_INDY_R(); c->a ^= RD(ea); set_nz(c, c->a); break;

    case 0x09: A_IMM();    c->a |= RD(ea); set_nz(c, c->a); break;
    case 0x05: A_ZP();     c->a |= RD(ea); set_nz(c, c->a); break;
    case 0x15: A_ZPX();    c->a |= RD(ea); set_nz(c, c->a); break;
    case 0x0D: A_ABS();    c->a |= RD(ea); set_nz(c, c->a); break;
    case 0x1D: A_ABSX_R(); c->a |= RD(ea); set_nz(c, c->a); break;
    case 0x19: A_ABSY_R(); c->a |= RD(ea); set_nz(c, c->a); break;
    case 0x01: A_INDX();   c->a |= RD(ea); set_nz(c, c->a); break;
    case 0x11: A_INDY_R(); c->a |= RD(ea); set_nz(c, c->a); break;

    /* BIT takes N and V straight from the memory operand, not the AND. */
    case 0x24: A_ZP();  v = RD(ea); goto do_bit;
    case 0x2C: A_ABS(); v = RD(ea);
    do_bit:
        set_flag(c, M6502_N, (v & 0x80u) != 0);
        set_flag(c, M6502_V, (v & 0x40u) != 0);
        set_flag(c, M6502_Z, (c->a & v) == 0);
        break;

    /* ---- arithmetic -------------------------------------------------- */
    case 0x69: A_IMM();    op_adc(c, RD(ea)); break;
    case 0x65: A_ZP();     op_adc(c, RD(ea)); break;
    case 0x75: A_ZPX();    op_adc(c, RD(ea)); break;
    case 0x6D: A_ABS();    op_adc(c, RD(ea)); break;
    case 0x7D: A_ABSX_R(); op_adc(c, RD(ea)); break;
    case 0x79: A_ABSY_R(); op_adc(c, RD(ea)); break;
    case 0x61: A_INDX();   op_adc(c, RD(ea)); break;
    case 0x71: A_INDY_R(); op_adc(c, RD(ea)); break;

    case 0xE9: A_IMM();    op_sbc(c, RD(ea)); break;
    case 0xE5: A_ZP();     op_sbc(c, RD(ea)); break;
    case 0xF5: A_ZPX();    op_sbc(c, RD(ea)); break;
    case 0xED: A_ABS();    op_sbc(c, RD(ea)); break;
    case 0xFD: A_ABSX_R(); op_sbc(c, RD(ea)); break;
    case 0xF9: A_ABSY_R(); op_sbc(c, RD(ea)); break;
    case 0xE1: A_INDX();   op_sbc(c, RD(ea)); break;
    case 0xF1: A_INDY_R(); op_sbc(c, RD(ea)); break;

    case 0xC9: A_IMM();    op_cmp(c, c->a, RD(ea)); break;
    case 0xC5: A_ZP();     op_cmp(c, c->a, RD(ea)); break;
    case 0xD5: A_ZPX();    op_cmp(c, c->a, RD(ea)); break;
    case 0xCD: A_ABS();    op_cmp(c, c->a, RD(ea)); break;
    case 0xDD: A_ABSX_R(); op_cmp(c, c->a, RD(ea)); break;
    case 0xD9: A_ABSY_R(); op_cmp(c, c->a, RD(ea)); break;
    case 0xC1: A_INDX();   op_cmp(c, c->a, RD(ea)); break;
    case 0xD1: A_INDY_R(); op_cmp(c, c->a, RD(ea)); break;

    case 0xE0: A_IMM(); op_cmp(c, c->x, RD(ea)); break;
    case 0xE4: A_ZP();  op_cmp(c, c->x, RD(ea)); break;
    case 0xEC: A_ABS(); op_cmp(c, c->x, RD(ea)); break;

    case 0xC0: A_IMM(); op_cmp(c, c->y, RD(ea)); break;
    case 0xC4: A_ZP();  op_cmp(c, c->y, RD(ea)); break;
    case 0xCC: A_ABS(); op_cmp(c, c->y, RD(ea)); break;

    /* ---- increment / decrement --------------------------------------- */
    case 0xE6: A_ZP();     v = RD(ea); WR(ea, v); v++; WR(ea, v); set_nz(c, v); break;
    case 0xF6: A_ZPX();    v = RD(ea); WR(ea, v); v++; WR(ea, v); set_nz(c, v); break;
    case 0xEE: A_ABS();    v = RD(ea); WR(ea, v); v++; WR(ea, v); set_nz(c, v); break;
    case 0xFE: A_ABSX_W(); v = RD(ea); WR(ea, v); v++; WR(ea, v); set_nz(c, v); break;

    case 0xC6: A_ZP();     v = RD(ea); WR(ea, v); v--; WR(ea, v); set_nz(c, v); break;
    case 0xD6: A_ZPX();    v = RD(ea); WR(ea, v); v--; WR(ea, v); set_nz(c, v); break;
    case 0xCE: A_ABS();    v = RD(ea); WR(ea, v); v--; WR(ea, v); set_nz(c, v); break;
    case 0xDE: A_ABSX_W(); v = RD(ea); WR(ea, v); v--; WR(ea, v); set_nz(c, v); break;

    case 0xE8: c->x++; set_nz(c, c->x); break;  /* INX */
    case 0xC8: c->y++; set_nz(c, c->y); break;  /* INY */
    case 0xCA: c->x--; set_nz(c, c->x); break;  /* DEX */
    case 0x88: c->y--; set_nz(c, c->y); break;  /* DEY */

    /* ---- shifts and rotates ------------------------------------------ */
    case 0x0A: c->a = op_asl(c, c->a); break;
    case 0x06: A_ZP();     RMW(op_asl); break;
    case 0x16: A_ZPX();    RMW(op_asl); break;
    case 0x0E: A_ABS();    RMW(op_asl); break;
    case 0x1E: A_ABSX_W(); RMW(op_asl); break;

    case 0x4A: c->a = op_lsr(c, c->a); break;
    case 0x46: A_ZP();     RMW(op_lsr); break;
    case 0x56: A_ZPX();    RMW(op_lsr); break;
    case 0x4E: A_ABS();    RMW(op_lsr); break;
    case 0x5E: A_ABSX_W(); RMW(op_lsr); break;

    case 0x2A: c->a = op_rol(c, c->a); break;
    case 0x26: A_ZP();     RMW(op_rol); break;
    case 0x36: A_ZPX();    RMW(op_rol); break;
    case 0x2E: A_ABS();    RMW(op_rol); break;
    case 0x3E: A_ABSX_W(); RMW(op_rol); break;

    case 0x6A: c->a = op_ror(c, c->a); break;
    case 0x66: A_ZP();     RMW(op_ror); break;
    case 0x76: A_ZPX();    RMW(op_ror); break;
    case 0x6E: A_ABS();    RMW(op_ror); break;
    case 0x7E: A_ABSX_W(); RMW(op_ror); break;

    /* ---- jumps and calls --------------------------------------------- */
    case 0x4C: c->pc = fetch16(m, c); break;   /* JMP abs */

    case 0x6C: {                               /* JMP (abs) */
        uint16_t ptr = fetch16(m, c);
        uint16_t lo = RD(ptr);
        /* The NMOS page wrap: the high byte comes from the same page. */
        uint16_t hi = RD((uint16_t)((ptr & 0xFF00u) | ((ptr + 1u) & 0x00FFu)));
        c->pc = (uint16_t)(lo | (hi << 8));
        break;
    }

    case 0x20: {                               /* JSR — pushes PC-1 */
        uint16_t lo = fetch8(m, c);
        uint16_t target_hi_pc = c->pc;         /* points at the high byte */
        push8(m, c, (uint8_t)(target_hi_pc >> 8));
        push8(m, c, (uint8_t)(target_hi_pc & 0xFFu));
        uint16_t hi = RD(target_hi_pc);
        c->pc = (uint16_t)(lo | (hi << 8));
        break;
    }

    case 0x60: {                               /* RTS */
        uint16_t lo = pull8(m, c);
        uint16_t hi = pull8(m, c);
        c->pc = (uint16_t)((lo | (hi << 8)) + 1u);
        break;
    }

    case 0x40: {                               /* RTI */
        c->p = (uint8_t)((pull8(m, c) & ~M6502_B) | M6502_U);
        uint16_t lo = pull8(m, c);
        uint16_t hi = pull8(m, c);
        c->pc = (uint16_t)(lo | (hi << 8));
        break;
    }

    case 0x00:                                 /* BRK */
        c->pc++;                               /* the signature byte */
        cyc = enter_interrupt(m, c, M6502_VEC_IRQ, true);
        break;

    /* ---- branches ---------------------------------------------------- */
    case 0x10: BRANCH(!(c->p & M6502_N)); break;  /* BPL */
    case 0x30: BRANCH( (c->p & M6502_N)); break;  /* BMI */
    case 0x50: BRANCH(!(c->p & M6502_V)); break;  /* BVC */
    case 0x70: BRANCH( (c->p & M6502_V)); break;  /* BVS */
    case 0x90: BRANCH(!(c->p & M6502_C)); break;  /* BCC */
    case 0xB0: BRANCH( (c->p & M6502_C)); break;  /* BCS */
    case 0xD0: BRANCH(!(c->p & M6502_Z)); break;  /* BNE */
    case 0xF0: BRANCH( (c->p & M6502_Z)); break;  /* BEQ */

    /* ---- flags ------------------------------------------------------- */
    case 0x18: c->p = (uint8_t)(c->p & ~M6502_C); break;  /* CLC */
    case 0x38: c->p |= M6502_C; break;                    /* SEC */
    case 0x58: c->p = (uint8_t)(c->p & ~M6502_I); break;  /* CLI */
    case 0x78: c->p |= M6502_I; break;                    /* SEI */
    case 0xB8: c->p = (uint8_t)(c->p & ~M6502_V); break;  /* CLV */
    case 0xD8: c->p = (uint8_t)(c->p & ~M6502_D); break;  /* CLD */
    case 0xF8: c->p |= M6502_D; break;                    /* SED */

    case 0xEA: break;                                     /* NOP */

    default:
        /* Undocumented: trap and log (§6.1). The stable subset lands in
         * v2; Atom ROMs do not use any of them. Consumed as a two-cycle
         * NOP so a stray byte cannot wedge the interpreter. */
        c->undoc_count++;
        c->undoc_pc = (uint16_t)(c->pc - 1u);
        c->undoc_op = op;
        cyc = 2u;
        break;
    }

#undef A_IMM
#undef A_ZP
#undef A_ZPX
#undef A_ZPY
#undef A_ABS
#undef A_ABSX_R
#undef A_ABSY_R
#undef A_ABSX_W
#undef A_ABSY_W
#undef A_INDX
#undef A_INDY_R
#undef A_INDY_W
#undef RMW
#undef BRANCH

    c->cycles += cyc;
    return cyc;
}
