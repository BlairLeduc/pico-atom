/* test_m6502_functional.c — runs Klaus Dormann's 6502 functional test and
 * Bruce Clark's decimal test (design.md §15.1). Both are non-negotiable:
 * they are the difference between an emulator and a plausible one.
 *
 * Neither binary is in the tree. Build them from
 * github.com/Klaus2m5/6502_65C02_functional_tests and point
 * PICO_ATOM_TEST_ROMS at the directory holding:
 *
 *   6502_functional_test.bin    64 KiB image, load #0000, start #0400
 *   6502_decimal_test.bin       load #0200, start #0200, result in #000B
 *
 * Those load and start addresses are the defaults of that repository's
 * supplied listings; if you assemble with different options, override
 * them with the environment variables named below. Without the binaries
 * the test reports as skipped rather than as passing.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atom.h"
#include "bus.h"
#include "m6502.h"
#include "test_util.h"

static atom_t g_machine;

static unsigned env_addr(const char *name, unsigned dflt) {
    const char *s = getenv(name);
    if (!s || !*s) return dflt;
    return (unsigned)strtoul(s, NULL, 0);
}

static long load_file(const char *path, uint8_t *dst, size_t cap) {
    FILE *f = fopen(path, "rb");
    if (!f) return -1;
    size_t n = fread(dst, 1, cap, f);
    fclose(f);
    return (long)n;
}

/* Run until the program traps — a branch or jump to itself, which is how
 * both suites signal both success and failure. Returns the trap PC, or
 * 0xFFFFFFFF if the instruction budget ran out first. */
static unsigned long run_to_trap(atom_t *m, unsigned long max_instructions) {
    for (unsigned long i = 0; i < max_instructions; i++) {
        uint16_t before = m->cpu.pc;
        m6502_step(m);
        if (m->cpu.pc == before) return before;
    }
    return 0xFFFFFFFFul;
}

static void prepare(atom_t *m) {
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(m, &cfg);
    /* The suites need a bare 64 KiB of RAM: no ROM, no I/O, no mirrors. */
    atom_map_ram(m, 0x0000, ATOM_ADDR_SPACE);
    memset(m->ram, 0, sizeof(m->ram));
    m6502_init(&m->cpu);
    m->cpu.s = 0xFF;
}

int main(void) {
    const char *dir = getenv("PICO_ATOM_TEST_ROMS");
#ifdef PICO_ATOM_DEFAULT_TEST_ROMS
    if (!dir || !*dir) dir = PICO_ATOM_DEFAULT_TEST_ROMS;
#endif
    if (!dir || !*dir) {
        printf("PICO_ATOM_TEST_ROMS is not set; skipping\n");
        return TEST_SKIP_CODE;
    }

    char path[512];
    int ran_any = 0;

    /* ---- Klaus Dormann, 6502_functional_test ------------------------ */
    snprintf(path, sizeof(path), "%s/6502_functional_test.bin", dir);
    {
        prepare(&g_machine);
        long n = load_file(path, g_machine.ram, sizeof(g_machine.ram));
        if (n < 0) {
            printf("%s not found; skipping the functional test\n", path);
        } else {
            ran_any = 1;
            unsigned start   = env_addr("PICO_ATOM_FUNCTIONAL_START", 0x0400);
            unsigned success = env_addr("PICO_ATOM_FUNCTIONAL_SUCCESS", 0x3469);
            g_machine.cpu.pc = (uint16_t)start;

            unsigned long trap = run_to_trap(&g_machine, 500000000ul);
            CHECK(trap != 0xFFFFFFFFul, "functional test did not trap within the budget");
            CHECK(trap == success,
                  "functional test trapped at #%04lX, expected the success trap at #%04X "
                  "(if you assembled it yourself, set PICO_ATOM_FUNCTIONAL_SUCCESS)",
                  trap, success);
            printf("functional test: trap at #%04lX after %llu cycles\n",
                   trap, (unsigned long long)g_machine.cpu.cycles);
            CHECK(g_machine.cpu.undoc_count == 0,
                  "%u undocumented opcode(s) executed, last 0x%02X at #%04X",
                  g_machine.cpu.undoc_count, g_machine.cpu.undoc_op, g_machine.cpu.undoc_pc);
        }
    }

    /* ---- Bruce Clark, decimal mode ---------------------------------- */
    snprintf(path, sizeof(path), "%s/6502_decimal_test.bin", dir);
    {
        prepare(&g_machine);
        unsigned load  = env_addr("PICO_ATOM_DECIMAL_LOAD",  0x0200);
        unsigned start = env_addr("PICO_ATOM_DECIMAL_START", 0x0200);
        unsigned errloc = env_addr("PICO_ATOM_DECIMAL_ERROR", 0x000B);

        long n = load_file(path, &g_machine.ram[load], sizeof(g_machine.ram) - load);
        if (n < 0) {
            printf("%s not found; skipping the decimal test\n", path);
        } else {
            ran_any = 1;
            g_machine.cpu.pc = (uint16_t)start;
            unsigned long trap = run_to_trap(&g_machine, 500000000ul);
            CHECK(trap != 0xFFFFFFFFul, "decimal test did not trap within the budget");
            CHECK(g_machine.ram[errloc] == 0,
                  "decimal test reports error code 0x%02X at #%04X (trapped at #%04lX)",
                  g_machine.ram[errloc], errloc, trap);
            printf("decimal test: trap at #%04lX, error byte 0x%02X\n",
                   trap, g_machine.ram[errloc]);
        }
    }

    if (!ran_any) {
        printf("no suite binaries found under %s; skipping\n", dir);
        return TEST_SKIP_CODE;
    }

    TEST_DONE();
}
