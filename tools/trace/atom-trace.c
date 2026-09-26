/* atom-trace.c — pico-atom's core on the host, printing one line per
 * instruction: our half of §15.1's trace-diff harness (design.md).
 *
 *   atom-trace KERNEL BASIC FLOAT DOS [-n INSNS] [-f FIELDS] [-k KEYS] [-s]
 *
 * -s prints the alpha screen on stderr at the end.
 * Each line is the state before an instruction: PC A X Y S P, cycles, and
 * the three bytes at PC, as atomulator-trace prints it. Interrupt entry is
 * a step of its own in m6502_step but not an instruction, so it has no
 * line; the handler's first instruction does. P is printed with bits 4 and 5 set, because
 * Atomulator keeps neither.
 *
 * The machine is atom_config_default's, with no utility ROM and RND left
 * at zero, and it is run as atom_run_field runs it (§12.1), one
 * instruction at a time: atom_run(m, 1) executes exactly one step.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atom.h"
#include "m6502.h"
#include "keyscript.h"

static atom_t machine;
static keyscript_t ks;
static unsigned long long insns, max_insns = 2000000ull;
static bool shift, ctrl, rept;

/* Through the page table, not bus_read, which would move open_bus. Code
 * never runs from I/O, so there is a page; 0 if not. */
static unsigned peek(const atom_t *m, uint16_t a) {
    const uint8_t *page = m->page[a >> 8].read;
    return page ? page[a & 0xFFu] : 0u;
}

static void press(atom_t *m, const ks_event_t *e);

static void step(atom_t *m) {
    const m6502_t *c = &m->cpu;
    static bool started;
    static uint64_t base;
    if (!started) { base = c->cycles; started = true; }

    const ks_event_t *e;
    while ((e = ks_due(&ks, c->cycles - base)) != NULL) press(m, e);

    bool entry = c->reset_pending || c->nmi_pending ||
                 (c->irq_lines != 0 && !(c->p & M6502_I));
    if (!entry) {
        printf("%04X %02X %02X %02X %02X %02X %llu %02X%02X%02X\n", c->pc, c->a, c->x,
               c->y, c->s, (unsigned)((c->p & 0xCFu) | 0x30u), (unsigned long long)c->cycles,
               peek(m, c->pc), peek(m, (uint16_t)(c->pc + 1)), peek(m, (uint16_t)(c->pc + 2)));
        if (++insns >= max_insns) { fflush(stdout); exit(0); }
    }
    m->budget -= (int32_t)atom_run(m, 1);
}

/* run_budget in atom.c, an instruction at a time. */
static void spend(atom_t *m, uint32_t cycles) {
    m->budget += (int32_t)cycles;
    while (m->budget > 0) step(m);
}

/* atom_run_field (atom.c), unrolled so that every instruction is seen. */
static void run_field(atom_t *m) {
    spend(m, m->field_active);
    atom_field_sync(m, true);
    spend(m, m->field_fs_low);
    atom_field_sync(m, false);
    spend(m, m->field_blank);
    atom_cassette_sync(m);
}

static void press(atom_t *m, const ks_event_t *e) {
    switch (e->kind) {
    case KS_CELL:  atom_key_set(m, (uint8_t)e->bit, (uint8_t)e->col, e->down); return;
    case KS_SHIFT: shift = e->down; break;
    case KS_CTRL:  ctrl  = e->down; break;
    case KS_REPT:  rept  = e->down; break;
    }
    atom_key_mods(m, shift, ctrl, rept);
}

/* The alpha screen on stderr, for the runner to show what was typed:
 * inverse as the plain character, graphics as '#' (design.md §2.4). */
static void dump_screen(const atom_t *m) {
    const uint8_t *v = atom_vram(m);
    for (int row = 0; row < 16; row++) {
        char line[33];
        int end = 0;
        for (int col = 0; col < 32; col++) {
            uint8_t b = v[row * 32 + col];
            char ch = (b & 0x40) ? '#' : (char)((b & 0x3F) < 0x20 ? (b & 0x3F) + 0x40 : (b & 0x3F));
            line[col] = ch;
            if (ch != ' ') end = col + 1;
        }
        line[end] = '\0';
        fprintf(stderr, "%s\n", line);
    }
}

static bool load(atom_t *m, const char *path, uint16_t addr) {
    static uint8_t buf[4096];
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "%s: cannot open\n", path); return false; }
    size_t n = fread(buf, 1, sizeof buf, f);
    fclose(f);
    if (n != sizeof buf) { fprintf(stderr, "%s: not 4 KiB\n", path); return false; }
    return atom_load_rom(m, addr, buf, n);
}

int main(int argc, char **argv) {
    const char *keyfile = NULL;
    unsigned long max_fields = 0;
    bool screen = false;
    const char *romv[4];
    int nrom = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-n") && i + 1 < argc)      max_insns  = strtoull(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-f") && i + 1 < argc) max_fields = strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "-k") && i + 1 < argc) keyfile    = argv[++i];
        else if (!strcmp(argv[i], "-s"))                 screen     = true;
        else if (nrom < 4)                               romv[nrom++] = argv[i];
        else nrom = 5;
    }
    if (nrom != 4) {
        fprintf(stderr, "usage: %s KERNEL BASIC FLOAT DOS [-n INSNS] [-f FIELDS] [-k KEYS] [-s]\n", argv[0]);
        return 2;
    }
    if (!ks_load(&ks, keyfile)) return 2;

    atom_t *m = &machine;
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(m, &cfg);
    if (!load(m, romv[0], 0xF000) || !load(m, romv[1], 0xC000) ||
        !load(m, romv[2], 0xD000) || !load(m, romv[3], 0xE000)) return 2;
    atom_reset(m);

    for (unsigned long field = 0; !max_fields || field < max_fields; field++) run_field(m);
    fflush(stdout);
    if (screen) dump_screen(m);
    return 0;
}
