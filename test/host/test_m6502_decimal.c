/* test_m6502_decimal.c — decimal mode, exhaustively (design.md §15.1).
 *
 * Bruce Clark's test is a 6502 program and is run by
 * test_m6502_functional when its binary is supplied. This test is the
 * part that can be asserted without one: for every pair of *valid* BCD
 * operands and both carry values, the result and the carry must be the
 * BCD arithmetic answer. That expectation is derived from what BCD
 * means, not from the interpreter, so it is an independent check.
 *
 * Invalid-BCD inputs are deliberately not asserted here; their NMOS
 * behaviour is exactly what the Clark ROM exists to pin down.
 *
 * Binary mode is checked exhaustively in the same shape.
 */

#include <string.h>

#include "atom.h"
#include "bus.h"
#include "m6502.h"
#include "test_util.h"

static atom_t g_machine;

/* Run one immediate-mode instruction and return the resulting CPU. */
static m6502_t run_imm(uint8_t opcode, uint8_t a, uint8_t operand,
                       bool carry_in, bool decimal) {
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&g_machine, &cfg);
    atom_map_ram(&g_machine, 0x0000, ATOM_ADDR_SPACE);
    memset(g_machine.ram, 0, sizeof(g_machine.ram));

    m6502_t *c = &g_machine.cpu;
    m6502_init(c);
    c->s = 0xFF;
    c->p = M6502_U;
    if (carry_in) c->p |= M6502_C;
    if (decimal)  c->p |= M6502_D;
    c->pc = 0x1000;
    c->a = a;

    g_machine.ram[0x1000] = opcode;
    g_machine.ram[0x1001] = operand;
    m6502_step(&g_machine);
    return *c;
}

static bool is_bcd(uint8_t v) { return (v & 0x0Fu) <= 9u && (v >> 4) <= 9u; }
static int  from_bcd(uint8_t v) { return (v >> 4) * 10 + (v & 0x0Fu); }
static uint8_t to_bcd(int n) { return (uint8_t)(((n / 10) << 4) | (n % 10)); }

int main(void) {
    /* ---- decimal ADC ------------------------------------------------- */
    for (int a = 0; a < 256; a++) {
        if (!is_bcd((uint8_t)a)) continue;
        for (int v = 0; v < 256; v++) {
            if (!is_bcd((uint8_t)v)) continue;
            for (int cin = 0; cin < 2; cin++) {
                m6502_t c = run_imm(0x69, (uint8_t)a, (uint8_t)v, cin != 0, true);
                int sum = from_bcd((uint8_t)a) + from_bcd((uint8_t)v) + cin;
                uint8_t want = to_bcd(sum % 100);
                bool want_c = sum >= 100;
                CHECK(c.a == want, "SED; ADC: 0x%02X + 0x%02X + %d = 0x%02X, expected 0x%02X",
                      a, v, cin, c.a, want);
                CHECK(((c.p & M6502_C) != 0) == want_c,
                      "SED; ADC: 0x%02X + 0x%02X + %d carry should be %d",
                      a, v, cin, want_c);
            }
        }
    }

    /* ---- decimal SBC ------------------------------------------------- */
    for (int a = 0; a < 256; a++) {
        if (!is_bcd((uint8_t)a)) continue;
        for (int v = 0; v < 256; v++) {
            if (!is_bcd((uint8_t)v)) continue;
            for (int cin = 0; cin < 2; cin++) {
                m6502_t c = run_imm(0xE9, (uint8_t)a, (uint8_t)v, cin != 0, true);
                int diff = from_bcd((uint8_t)a) - from_bcd((uint8_t)v) - (1 - cin);
                uint8_t want = to_bcd(((diff % 100) + 100) % 100);
                bool want_c = diff >= 0;
                CHECK(c.a == want, "SED; SBC: 0x%02X - 0x%02X - %d = 0x%02X, expected 0x%02X",
                      a, v, 1 - cin, c.a, want);
                CHECK(((c.p & M6502_C) != 0) == want_c,
                      "SED; SBC: 0x%02X - 0x%02X - %d borrow should be %d",
                      a, v, 1 - cin, !want_c);
            }
        }
    }

    /* On the NMOS part SBC takes all four flags from the binary
     * operation, decimal mode or not — only the result is adjusted. */
    for (int a = 0; a < 256; a += 7) {
        for (int v = 0; v < 256; v += 11) {
            for (int cin = 0; cin < 2; cin++) {
                m6502_t bin = run_imm(0xE9, (uint8_t)a, (uint8_t)v, cin != 0, false);
                m6502_t dec = run_imm(0xE9, (uint8_t)a, (uint8_t)v, cin != 0, true);
                uint8_t mask = M6502_N | M6502_V | M6502_Z | M6502_C;
                CHECK((bin.p & mask) == (dec.p & mask),
                      "SBC flags should not depend on D: 0x%02X - 0x%02X - %d, "
                      "binary P=0x%02X decimal P=0x%02X",
                      a, v, 1 - cin, bin.p & mask, dec.p & mask);
            }
        }
    }

    /* ---- binary ADC and SBC, exhaustively ---------------------------- */
    for (int a = 0; a < 256; a++) {
        for (int v = 0; v < 256; v++) {
            for (int cin = 0; cin < 2; cin++) {
                int sum = a + v + cin;
                uint8_t want = (uint8_t)sum;
                bool want_v = (~(a ^ v) & (a ^ sum) & 0x80) != 0;
                m6502_t c = run_imm(0x69, (uint8_t)a, (uint8_t)v, cin != 0, false);
                CHECK(c.a == want, "ADC 0x%02X + 0x%02X + %d", a, v, cin);
                CHECK(((c.p & M6502_C) != 0) == (sum > 0xFF), "ADC carry 0x%02X+0x%02X", a, v);
                CHECK(((c.p & M6502_V) != 0) == want_v, "ADC overflow 0x%02X+0x%02X", a, v);
                CHECK(((c.p & M6502_Z) != 0) == (want == 0), "ADC zero 0x%02X+0x%02X", a, v);
                CHECK(((c.p & M6502_N) != 0) == ((want & 0x80) != 0),
                      "ADC negative 0x%02X+0x%02X", a, v);

                int diff = a - v - (1 - cin);
                uint8_t wantd = (uint8_t)diff;
                bool want_vd = ((a ^ v) & (a ^ diff) & 0x80) != 0;
                m6502_t d = run_imm(0xE9, (uint8_t)a, (uint8_t)v, cin != 0, false);
                CHECK(d.a == wantd, "SBC 0x%02X - 0x%02X - %d", a, v, 1 - cin);
                CHECK(((d.p & M6502_C) != 0) == (diff >= 0), "SBC carry 0x%02X-0x%02X", a, v);
                CHECK(((d.p & M6502_V) != 0) == want_vd, "SBC overflow 0x%02X-0x%02X", a, v);
            }
        }
    }

    TEST_DONE();
}
