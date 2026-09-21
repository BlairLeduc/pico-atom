/* test_m6502_cycles.c — every documented opcode's cycle count and the
 * page-cross and branch penalties, asserted against the published table
 * (design.md §15.1).
 *
 * The expectations below are transcribed in instruction order, grouped by
 * mnemonic, rather than as the 16x16 grid the interpreter carries. Two
 * differently shaped transcriptions of the same table catch different
 * slips; that is the point of writing it out twice.
 *
 * Each expectation is checked by *executing* the opcode, not by comparing
 * tables, so the addressing-mode code is on the hook too.
 */

#include <string.h>

#include "atom.h"
#include "bus.h"
#include "m6502.h"
#include "test_util.h"

typedef struct { uint8_t op; uint8_t cycles; const char *name; } expect_t;

static const expect_t expected[] = {
    /* load / store */
    {0xA9, 2, "LDA #"},   {0xA5, 3, "LDA zp"},   {0xB5, 4, "LDA zp,X"},
    {0xAD, 4, "LDA abs"}, {0xBD, 4, "LDA abs,X"},{0xB9, 4, "LDA abs,Y"},
    {0xA1, 6, "LDA (zp,X)"}, {0xB1, 5, "LDA (zp),Y"},
    {0xA2, 2, "LDX #"},   {0xA6, 3, "LDX zp"},   {0xB6, 4, "LDX zp,Y"},
    {0xAE, 4, "LDX abs"}, {0xBE, 4, "LDX abs,Y"},
    {0xA0, 2, "LDY #"},   {0xA4, 3, "LDY zp"},   {0xB4, 4, "LDY zp,X"},
    {0xAC, 4, "LDY abs"}, {0xBC, 4, "LDY abs,X"},
    {0x85, 3, "STA zp"},  {0x95, 4, "STA zp,X"}, {0x8D, 4, "STA abs"},
    {0x9D, 5, "STA abs,X"}, {0x99, 5, "STA abs,Y"},
    {0x81, 6, "STA (zp,X)"}, {0x91, 6, "STA (zp),Y"},
    {0x86, 3, "STX zp"},  {0x96, 4, "STX zp,Y"}, {0x8E, 4, "STX abs"},
    {0x84, 3, "STY zp"},  {0x94, 4, "STY zp,X"}, {0x8C, 4, "STY abs"},

    /* transfers */
    {0xAA, 2, "TAX"}, {0xA8, 2, "TAY"}, {0xBA, 2, "TSX"},
    {0x8A, 2, "TXA"}, {0x9A, 2, "TXS"}, {0x98, 2, "TYA"},

    /* stack */
    {0x48, 3, "PHA"}, {0x08, 3, "PHP"}, {0x68, 4, "PLA"}, {0x28, 4, "PLP"},

    /* logic */
    {0x29, 2, "AND #"},   {0x25, 3, "AND zp"},   {0x35, 4, "AND zp,X"},
    {0x2D, 4, "AND abs"}, {0x3D, 4, "AND abs,X"},{0x39, 4, "AND abs,Y"},
    {0x21, 6, "AND (zp,X)"}, {0x31, 5, "AND (zp),Y"},
    {0x49, 2, "EOR #"},   {0x45, 3, "EOR zp"},   {0x55, 4, "EOR zp,X"},
    {0x4D, 4, "EOR abs"}, {0x5D, 4, "EOR abs,X"},{0x59, 4, "EOR abs,Y"},
    {0x41, 6, "EOR (zp,X)"}, {0x51, 5, "EOR (zp),Y"},
    {0x09, 2, "ORA #"},   {0x05, 3, "ORA zp"},   {0x15, 4, "ORA zp,X"},
    {0x0D, 4, "ORA abs"}, {0x1D, 4, "ORA abs,X"},{0x19, 4, "ORA abs,Y"},
    {0x01, 6, "ORA (zp,X)"}, {0x11, 5, "ORA (zp),Y"},
    {0x24, 3, "BIT zp"},  {0x2C, 4, "BIT abs"},

    /* arithmetic */
    {0x69, 2, "ADC #"},   {0x65, 3, "ADC zp"},   {0x75, 4, "ADC zp,X"},
    {0x6D, 4, "ADC abs"}, {0x7D, 4, "ADC abs,X"},{0x79, 4, "ADC abs,Y"},
    {0x61, 6, "ADC (zp,X)"}, {0x71, 5, "ADC (zp),Y"},
    {0xE9, 2, "SBC #"},   {0xE5, 3, "SBC zp"},   {0xF5, 4, "SBC zp,X"},
    {0xED, 4, "SBC abs"}, {0xFD, 4, "SBC abs,X"},{0xF9, 4, "SBC abs,Y"},
    {0xE1, 6, "SBC (zp,X)"}, {0xF1, 5, "SBC (zp),Y"},
    {0xC9, 2, "CMP #"},   {0xC5, 3, "CMP zp"},   {0xD5, 4, "CMP zp,X"},
    {0xCD, 4, "CMP abs"}, {0xDD, 4, "CMP abs,X"},{0xD9, 4, "CMP abs,Y"},
    {0xC1, 6, "CMP (zp,X)"}, {0xD1, 5, "CMP (zp),Y"},
    {0xE0, 2, "CPX #"},   {0xE4, 3, "CPX zp"},   {0xEC, 4, "CPX abs"},
    {0xC0, 2, "CPY #"},   {0xC4, 3, "CPY zp"},   {0xCC, 4, "CPY abs"},

    /* increment / decrement */
    {0xE6, 5, "INC zp"},  {0xF6, 6, "INC zp,X"}, {0xEE, 6, "INC abs"},
    {0xFE, 7, "INC abs,X"},
    {0xC6, 5, "DEC zp"},  {0xD6, 6, "DEC zp,X"}, {0xCE, 6, "DEC abs"},
    {0xDE, 7, "DEC abs,X"},
    {0xE8, 2, "INX"}, {0xC8, 2, "INY"}, {0xCA, 2, "DEX"}, {0x88, 2, "DEY"},

    /* shifts and rotates */
    {0x0A, 2, "ASL A"},   {0x06, 5, "ASL zp"},   {0x16, 6, "ASL zp,X"},
    {0x0E, 6, "ASL abs"}, {0x1E, 7, "ASL abs,X"},
    {0x4A, 2, "LSR A"},   {0x46, 5, "LSR zp"},   {0x56, 6, "LSR zp,X"},
    {0x4E, 6, "LSR abs"}, {0x5E, 7, "LSR abs,X"},
    {0x2A, 2, "ROL A"},   {0x26, 5, "ROL zp"},   {0x36, 6, "ROL zp,X"},
    {0x2E, 6, "ROL abs"}, {0x3E, 7, "ROL abs,X"},
    {0x6A, 2, "ROR A"},   {0x66, 5, "ROR zp"},   {0x76, 6, "ROR zp,X"},
    {0x6E, 6, "ROR abs"}, {0x7E, 7, "ROR abs,X"},

    /* jumps, calls, returns */
    {0x4C, 3, "JMP abs"}, {0x6C, 5, "JMP (abs)"},
    {0x20, 6, "JSR"},     {0x60, 6, "RTS"},
    {0x40, 6, "RTI"},     {0x00, 7, "BRK"},

    /* branches, not taken */
    {0x10, 2, "BPL"}, {0x30, 2, "BMI"}, {0x50, 2, "BVC"}, {0x70, 2, "BVS"},
    {0x90, 2, "BCC"}, {0xB0, 2, "BCS"}, {0xD0, 2, "BNE"}, {0xF0, 2, "BEQ"},

    /* flags and NOP */
    {0x18, 2, "CLC"}, {0x38, 2, "SEC"}, {0x58, 2, "CLI"}, {0x78, 2, "SEI"},
    {0xB8, 2, "CLV"}, {0xD8, 2, "CLD"}, {0xF8, 2, "SED"}, {0xEA, 2, "NOP"},
};

#define N_EXPECTED ((int)(sizeof(expected) / sizeof(expected[0])))

static atom_t g_machine;

/* A bare 64 KiB RAM machine — no ROM, no I/O — which is what the CPU
 * tests want. */
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

/* Put the branch flags where the branch will not be taken, so the opcode
 * costs its base two cycles. */
static void set_branch_not_taken(m6502_t *c, uint8_t op) {
    switch (op) {
    case 0x10: c->p |= M6502_N; break;                  /* BPL */
    case 0x30: c->p = (uint8_t)(c->p & ~M6502_N); break;/* BMI */
    case 0x50: c->p |= M6502_V; break;                  /* BVC */
    case 0x70: c->p = (uint8_t)(c->p & ~M6502_V); break;/* BVS */
    case 0x90: c->p |= M6502_C; break;                  /* BCC */
    case 0xB0: c->p = (uint8_t)(c->p & ~M6502_C); break;/* BCS */
    case 0xD0: c->p |= M6502_Z; break;                  /* BNE */
    case 0xF0: c->p = (uint8_t)(c->p & ~M6502_Z); break;/* BEQ */
    default: break;
    }
}

/* Execute one opcode at 0x1000 with operands that stay inside a page. */
static uint32_t run_one(uint8_t op, uint8_t o1, uint8_t o2,
                        uint8_t x, uint8_t y) {
    atom_t *m = bare_machine();
    m->ram[0x1000] = op;
    m->ram[0x1001] = o1;
    m->ram[0x1002] = o2;
    m->cpu.x = x;
    m->cpu.y = y;
    set_branch_not_taken(&m->cpu, op);
    return m6502_step(m);
}

int main(void) {
    /* The documented set is exactly 151 opcodes, and the table above
     * covers every one of them with no duplicates. */
    int documented = 0;
    for (int i = 0; i < 256; i++) if (m6502_is_documented((uint8_t)i)) documented++;
    CHECK(documented == 151, "interpreter claims %d documented opcodes", documented);
    CHECK(N_EXPECTED == 151, "expectation table has %d entries", N_EXPECTED);

    int seen[256] = {0};
    for (int i = 0; i < N_EXPECTED; i++) {
        uint8_t op = expected[i].op;
        CHECK(!seen[op], "opcode 0x%02X listed twice", op);
        seen[op] = 1;
        CHECK(m6502_is_documented(op), "0x%02X (%s) not documented by the interpreter",
              op, expected[i].name);
        CHECK(m6502_base_cycles(op) == expected[i].cycles,
              "0x%02X (%s): table says %u, expected %u",
              op, expected[i].name, m6502_base_cycles(op), expected[i].cycles);

        uint32_t got = run_one(op, 0x00, 0x02, 0, 0);
        CHECK(got == expected[i].cycles, "0x%02X (%s): executed in %u cycles, expected %u",
              op, expected[i].name, got, expected[i].cycles);
    }
    for (int i = 0; i < 256; i++) {
        CHECK(seen[i] == (m6502_is_documented((uint8_t)i) ? 1 : 0),
              "0x%02X: table and interpreter disagree about whether it is documented", i);
    }

    /* ---- page-crossing reads cost one extra cycle ------------------- */
    struct { uint8_t op; const char *name; uint8_t base; bool uses_y; } crossing_reads[] = {
        {0xBD, "LDA abs,X", 4, false},
        {0xB9, "LDA abs,Y", 4, true},
        {0xBE, "LDX abs,Y", 4, true},
        {0xBC, "LDY abs,X", 4, false},
        {0x3D, "AND abs,X", 4, false},
        {0x79, "ADC abs,Y", 4, true},
        {0xDD, "CMP abs,X", 4, false},
    };
    for (size_t i = 0; i < sizeof(crossing_reads) / sizeof(crossing_reads[0]); i++) {
        uint8_t op = crossing_reads[i].op;
        uint8_t idx = crossing_reads[i].uses_y ? 0 : 1;
        uint8_t idy = crossing_reads[i].uses_y ? 1 : 0;
        /* base #02FF + 1 crosses into page #03 */
        uint32_t crossed = run_one(op, 0xFF, 0x02, idx, idy);
        CHECK(crossed == crossing_reads[i].base + 1u,
              "%s crossing a page took %u, expected %u",
              crossing_reads[i].name, crossed, crossing_reads[i].base + 1u);
        /* base #0200 + 1 does not cross */
        uint32_t same = run_one(op, 0x00, 0x02, idx, idy);
        CHECK(same == crossing_reads[i].base,
              "%s inside a page took %u, expected %u",
              crossing_reads[i].name, same, crossing_reads[i].base);
    }

    /* (zp),Y crosses too: the pointer, not the operand, supplies the base. */
    {
        atom_t *m = bare_machine();
        m->ram[0x1000] = 0xB1; m->ram[0x1001] = 0x20;   /* LDA ($20),Y */
        m->ram[0x0020] = 0xFF; m->ram[0x0021] = 0x02;   /* -> #02FF     */
        m->cpu.y = 1;
        CHECK(m6502_step(m) == 6, "LDA (zp),Y crossing a page should cost 6");

        m = bare_machine();
        m->ram[0x1000] = 0xB1; m->ram[0x1001] = 0x20;
        m->ram[0x0020] = 0x00; m->ram[0x0021] = 0x02;   /* -> #0200     */
        m->cpu.y = 1;
        CHECK(m6502_step(m) == 5, "LDA (zp),Y inside a page should cost 5");
    }

    /* ---- stores and read-modify-writes never gain the extra cycle --- */
    {
        uint32_t n = run_one(0x9D, 0xFF, 0x02, 1, 0);   /* STA abs,X */
        CHECK(n == 5, "STA abs,X crossing a page took %u, expected 5", n);
        n = run_one(0x99, 0xFF, 0x02, 0, 1);            /* STA abs,Y */
        CHECK(n == 5, "STA abs,Y crossing a page took %u, expected 5", n);
        n = run_one(0x1E, 0xFF, 0x02, 1, 0);            /* ASL abs,X */
        CHECK(n == 7, "ASL abs,X crossing a page took %u, expected 7", n);
        n = run_one(0xFE, 0xFF, 0x02, 1, 0);            /* INC abs,X */
        CHECK(n == 7, "INC abs,X crossing a page took %u, expected 7", n);

        atom_t *m = bare_machine();
        m->ram[0x1000] = 0x91; m->ram[0x1001] = 0x20;   /* STA ($20),Y */
        m->ram[0x0020] = 0xFF; m->ram[0x0021] = 0x02;
        m->cpu.y = 1;
        CHECK(m6502_step(m) == 6, "STA (zp),Y crossing a page should still cost 6");
    }

    /* ---- branch penalties ------------------------------------------- */
    {
        /* Taken, staying in the page: 3. */
        atom_t *m = bare_machine();
        m->ram[0x1000] = 0xD0; m->ram[0x1001] = 0x10;   /* BNE +16 */
        CHECK(m6502_step(m) == 3, "branch taken within a page should cost 3");
        CHECK(m->cpu.pc == 0x1012, "BNE +16 from #1000 should land at #1012, got #%04X",
              m->cpu.pc);

        /* Taken forwards across a page boundary: 4. */
        m = bare_machine();
        m->cpu.pc = 0x10FD;
        m->ram[0x10FD] = 0xD0; m->ram[0x10FE] = 0x01;   /* BNE +1 -> #1100 */
        CHECK(m6502_step(m) == 4, "branch crossing a page forwards should cost 4");
        CHECK(m->cpu.pc == 0x1100, "expected #1100, got #%04X", m->cpu.pc);

        /* Taken backwards across a page boundary: 4. */
        m = bare_machine();
        m->ram[0x1000] = 0xD0; m->ram[0x1001] = 0xFC;   /* BNE -4 -> #0FFE */
        CHECK(m6502_step(m) == 4, "branch crossing a page backwards should cost 4");
        CHECK(m->cpu.pc == 0x0FFE, "expected #0FFE, got #%04X", m->cpu.pc);

        /* Not taken: 2, and the operand is still consumed. */
        m = bare_machine();
        m->cpu.p |= M6502_Z;
        m->ram[0x1000] = 0xD0; m->ram[0x1001] = 0x10;
        CHECK(m6502_step(m) == 2, "branch not taken should cost 2");
        CHECK(m->cpu.pc == 0x1002, "not-taken branch should leave PC at #1002, got #%04X",
              m->cpu.pc);
    }

    /* ---- interrupt entry is seven cycles ---------------------------- */
    {
        atom_t *m = bare_machine();
        m->ram[0x1000] = 0xEA;
        m->cpu.p = (uint8_t)(m->cpu.p & ~M6502_I);
        m6502_set_irq(&m->cpu, M6502_IRQ_VIA, true);
        CHECK(m6502_step(m) == 7, "IRQ entry should cost 7 cycles");

        m = bare_machine();
        m6502_set_nmi(&m->cpu, true);
        CHECK(m6502_step(m) == 7, "NMI entry should cost 7 cycles");
    }

    TEST_DONE();
}
