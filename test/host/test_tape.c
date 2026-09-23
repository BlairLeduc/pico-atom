/* test_tape.c — tape phase 1: the OSLOAD/OSSAVE traps (design.md §11.2).
 *
 * The reference is the kernel ROM itself. Its own OSSAVE runs with the
 * byte-level cassette routines hooked here, so the bytes it would have
 * recorded are captured; its own OSLOAD then reads that stream back. The
 * trapped calls must leave the machine as those runs did: same memory,
 * same page zero, same stack and flags, same place to return to. Only
 * the bit-level scratch the trap never touches is exempt (NOT_COMPARED).
 *
 * Then the traps are driven the way a user drives them, from BASIC and
 * from *RUN, through the keyboard.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "guest.h"
#include "tape.h"
#include "test_util.h"

static guest_t g;
static atom_t  booted;

/* ---- the reference: the ROM's routines over a byte stream ---------- */

/* The cassette byte routines, reached through (#0214) and (#0216), and
 * the two places that wait on a signal no host can produce: the leader
 * tone (#FB8E) and the "PLAY TAPE" / "RECORD TAPE" keypress (#FC40). */
#define ROM_BGET     0xFBEEu
#define ROM_BPUT     0xFC7Cu
#define ROM_TONE     0xFB8Eu
#define ROM_TONE_OK  0xFBB4u   /* tone found: read the four sync bytes */
#define ROM_PROMPT   0xFC40u

#define ZP_SUM       0xDCu

/* Where a call returns to: RAM nothing else uses, never executed. */
#define RET          0x2FF0u

static uint8_t  stream[70000];
static size_t   stream_len, stream_pos;

static uint8_t peek(atom_t *m, uint16_t a) { return m->ram[a]; }

static void rts(atom_t *m) {
    m->cpu.s++;
    uint16_t lo = peek(m, (uint16_t)(0x100u | m->cpu.s));
    m->cpu.s++;
    uint16_t hi = peek(m, (uint16_t)(0x100u | m->cpu.s));
    m->cpu.pc = (uint16_t)(((hi << 8) | lo) + 1u);
}

/* #FC23: A added into the running checksum; the caller's flags come back
 * from PLP, then CLC/ADC/PLA leave C from the add and N/Z from A. */
static void add_sum(atom_t *m, uint8_t a) {
    unsigned s = (unsigned)m->ram[ZP_SUM] + a;
    m->ram[ZP_SUM] = (uint8_t)s;
    m->cpu.p = (uint8_t)(m->cpu.p & ~(M6502_C | M6502_N | M6502_Z));
    if (s > 0xFFu) m->cpu.p |= M6502_C;
    if (a & 0x80u) m->cpu.p |= M6502_N;
    if (a == 0)    m->cpu.p |= M6502_Z;
}

/* One instruction boundary's worth of hooks. True if one fired. */
static bool hook(atom_t *m) {
    switch (m->cpu.pc) {
    case ROM_BGET: {
        if (stream_pos >= stream_len) return false;   /* runs dry: hangs */
        uint8_t a = stream[stream_pos++];
        m->cpu.a = a;
        add_sum(m, a);
        rts(m);
        return true;
    }
    case ROM_BPUT: {
        uint8_t p = m->cpu.p;
        if (stream_len < sizeof stream) stream[stream_len++] = m->cpu.a;
        add_sum(m, m->cpu.a);
        m->cpu.p = p;                                  /* PLP at #FCBB */
        rts(m);
        return true;
    }
    case ROM_TONE:
        m->cpu.p |= M6502_V;
        m->cpu.pc = ROM_TONE_OK;
        return true;
    case ROM_PROMPT:
        bus_write(m, 0xB002u, 0x07u);
        rts(m);
        return true;
    default:
        return false;
    }
}

/* ---- serving a trapped request, as a port would -------------------- */

static uint8_t  atm[70000];
static size_t   atm_len;

static void serve(atom_t *m) {
    const tape_t *t = atom_tape_pending(m);
    if (!t) return;
    if (t->op == TAPE_SAVE) {
        atm_header_t h;
        atom_tape_save_header(m, &h);
        atm_header_encode(&h, atm);
        atm_len = ATM_HEADER_LEN + atom_tape_save_data(m, 0, atm + ATM_HEADER_LEN,
                                                       sizeof atm - ATM_HEADER_LEN);
        atom_tape_save_end(m);
    } else {
        atm_header_t h;
        atm_header_decode(atm, &h);
        if (atm_len < ATM_HEADER_LEN || !atm_name_matches(&h, t->name)) {
            atom_tape_decline(m);
            return;
        }
        atom_tape_load_begin(m, &h);
        /* In pieces, as the device does from the card. */
        for (size_t off = ATM_HEADER_LEN; off < atm_len; off += 100) {
            size_t n = atm_len - off < 100 ? atm_len - off : 100;
            atom_tape_load_data(m, atm + off, n);
        }
        atom_tape_load_end(m);
    }
}

static void on_field(atom_t *m) {
    int16_t discard[ATOM_AUDIO_BUF_LEN];
    (void)atom_audio_drain(m, discard, ATOM_AUDIO_BUF_LEN);
    serve(m);
}

/* ---- calling the MOS ------------------------------------------------ */

/* The parameter block lives here in page zero, and the name at #0140
 * where the MOS's own *LOAD puts it. */
#define BLOCK        0x52u
#define NAME_AT      0x0140u

static void set_block(atom_t *m, const char *name, const uint16_t *words, int nwords) {
    size_t n = strlen(name);
    memcpy(&m->ram[NAME_AT], name, n);
    m->ram[NAME_AT + n] = 0x0D;
    m->ram[BLOCK] = (uint8_t)NAME_AT;
    m->ram[BLOCK + 1] = (uint8_t)(NAME_AT >> 8);
    for (int i = 0; i < nwords; i++) {
        m->ram[BLOCK + 2 + 2 * i] = (uint8_t)words[i];
        m->ram[BLOCK + 3 + 2 * i] = (uint8_t)(words[i] >> 8);
    }
}

/* JSR entry from RET - 1, with X at the block, and run until it returns:
 * whole fields split at flyback, like atom_run_field, but a boundary at a
 * time so the hooks see every one. */
static bool call(atom_t *m, uint16_t entry, bool reference) {
    m->cfg.tape_traps = !reference;
    m->cpu.s--;
    m->ram[0x100u | (uint8_t)(m->cpu.s + 1u)] = (uint8_t)((RET - 1u) >> 8);
    m->cpu.s--;
    m->ram[0x100u | (uint8_t)(m->cpu.s + 1u)] = (uint8_t)(RET - 1u);
    m->cpu.x = BLOCK;
    m->cpu.pc = entry;

    uint32_t per_field = atom_cycles_per_field(m);
    uint32_t flyback = atom_flyback_cycles(m);
    for (int field = 0; field < 60 * 600; field++) {
        for (uint32_t done = 0; done < per_field;) {
            atom_field_sync(m, done >= per_field - flyback);
            if (m->cpu.pc == RET) {
                atom_field_sync(m, false);
                m->cfg.tape_traps = true;
                return true;
            }
            if (reference && hook(m)) continue;
            done += atom_run(m, 1);
            if (!reference) serve(m);
        }
    }
    m->cfg.tape_traps = true;
    fprintf(stderr, "    stuck at #%04X\n", m->cpu.pc);
    return false;
}

/* ---- comparing machines --------------------------------------------- */

/* Bit-level scratch of the cassette routines, which the trap has no bits
 * to put in: #C0-#C5 hold the byte being shifted, bit and cycle counters
 * and the saved Y (#FBEE, #FC7C, #FB8E); #EC is the saved X. Everything
 * else in the machine is compared. */
static bool not_compared(uint16_t a, uint8_t s) {
    if (a >= 0xC0u && a <= 0xC5u) return true;
    if (a == 0xECu) return true;
    /* Below the stack pointer is whatever the routines pushed last. */
    if (a >= 0x100u && a <= (0x100u | s)) return true;
    return false;
}

static int diff(const char *what, const atom_t *want, const atom_t *got) {
    int n = 0;
    for (uint32_t a = 0; a < 0xA000u; a++) {
        if (not_compared((uint16_t)a, want->cpu.s)) continue;
        if (want->ram[a] != got->ram[a]) {
            if (n < 40) fprintf(stderr, "    %s: #%04X ROM %02X trap %02X\n", what,
                                (unsigned)a, want->ram[a], got->ram[a]);
            n++;
        }
    }
    const m6502_t *w = &want->cpu, *t = &got->cpu;
#define REG(r) if (w->r != t->r) { fprintf(stderr, "    %s: " #r " ROM %02X trap %02X\n", \
                                           what, (unsigned)w->r, (unsigned)t->r); n++; }
    REG(pc) REG(s) REG(p) REG(a) REG(x) REG(y)
#undef REG
    if (want->ppi.out_c != got->ppi.out_c) {
        fprintf(stderr, "    %s: port C ROM %02X trap %02X\n", what,
                want->ppi.out_c, got->ppi.out_c);
        n++;
    }
    return n;
}

/* ---- the cases ------------------------------------------------------ */

typedef struct {
    const char *name;
    uint16_t    start, len, reload, exec;
    bool        own_addr;       /* load at the file's address */
    uint16_t    load_at;        /* else here */
} tcase_t;

static uint32_t rng = 12345u;
static uint8_t rnd(void) {
    rng = rng * 1103515245u + 12345u;
    return (uint8_t)(rng >> 16);
}

static int run_case(const tcase_t *c) {
    /* The data, in a fresh machine. */
    static atom_t src;
    atom_copy(&src, &booted);
    for (uint32_t i = 0; i < c->len; i++) src.ram[c->start + i] = rnd();
    uint16_t words[4] = { c->reload, c->exec, c->start, (uint16_t)(c->start + c->len) };

    /* Save: the ROM's, then the trap's, from the same state. */
    static atom_t rs, ts;
    atom_copy(&rs, &src);
    set_block(&rs, c->name, words, 4);
    atom_copy(&ts, &rs);
    stream_len = 0;
    CHECK(call(&rs, 0xFFDDu, true), "%s: the ROM's OSSAVE did not return", c->name);
    atm_len = 0;
    CHECK(call(&ts, 0xFFDDu, false), "%s: the trapped OSSAVE did not return", c->name);
    CHECK(ts.tape.served == 1, "%s: the save was not trapped", c->name);

    /* The file the trap wrote is what the ROM recorded. */
    atm_header_t h;
    atm_header_decode(atm, &h);
    CHECK(strcmp(h.name, c->name) == 0, "%s: ATM name '%s'", c->name, h.name);
    CHECK(h.load == c->reload && h.exec == c->exec && h.len == c->len,
          "%s: ATM load %04X exec %04X len %u", c->name, h.load, h.exec, h.len);
    CHECK(atm_len == ATM_HEADER_LEN + c->len, "%s: ATM is %zu bytes", c->name, atm_len);
    CHECK(memcmp(atm + ATM_HEADER_LEN, &src.ram[c->start], c->len) == 0,
          "%s: ATM data differs from memory", c->name);
    {
        /* Every data byte on tape, in order, framing stripped. */
        static uint8_t data[70000];
        size_t pos = 0, nd = 0;
        size_t nl = strlen(c->name);
        while (pos < stream_len) {
            pos += 4 + nl + 1;                         /* ****, name, CR */
            uint8_t flags = stream[pos];
            uint8_t len1 = stream[pos + 3];
            pos += 8;
            if (flags & 0x40u) {
                memcpy(data + nd, stream + pos, len1 + 1u);
                nd += len1 + 1u;
                pos += len1 + 1u;
            }
            pos += 1;                                  /* checksum */
        }
        CHECK(nd == c->len && memcmp(data, &src.ram[c->start], c->len) == 0,
              "%s: the ROM recorded %zu data bytes, want %u", c->name, nd, c->len);
    }
    int d = diff("save", &rs, &ts);
    CHECK(d == 0, "%s: the trapped OSSAVE left %d difference(s)", c->name, d);

    /* Load: into a machine that has never seen the data. */
    uint16_t lw[2] = { c->load_at, (uint16_t)(c->own_addr ? 0x0000u : 0x00FFu) };
    static atom_t rl, tl;
    atom_copy(&rl, &booted);
    set_block(&rl, c->name, lw, 2);
    rl.ram[BLOCK + 5] = 0;
    atom_copy(&tl, &rl);
    stream_pos = 0;
    CHECK(call(&rl, 0xFFE0u, true), "%s: the ROM's OSLOAD did not return", c->name);
    CHECK(stream_pos == stream_len, "%s: the ROM read %zu of %zu bytes", c->name,
          stream_pos, stream_len);
    CHECK(call(&tl, 0xFFE0u, false), "%s: the trapped OSLOAD did not return", c->name);
    CHECK(tl.tape.served == 1, "%s: the load was not trapped", c->name);
    uint16_t at = c->own_addr ? c->reload : c->load_at;
    CHECK(memcmp(&tl.ram[at], &src.ram[c->start], c->len) == 0,
          "%s: the data did not land at #%04X", c->name, at);
    d = diff("load", &rl, &tl);
    CHECK(d == 0, "%s: the trapped OSLOAD left %d difference(s)", c->name, d);
    return 0;
}

int main(void) {
    const char *dir;
    if (!guest_find_roms(&dir)) {
        printf("skipped: no kernel and BASIC images in %s\n", dir);
        return TEST_SKIP_CODE;
    }
    guest_boot(&g);
    atom_copy(&booted, &g.m);

    /* ATM round trip. */
    {
        atm_header_t h = { .name = "SIXTEEN-CHARS-AB", .load = 0x2900, .exec = 0xC2B2,
                           .len = 0x1234 };
        uint8_t raw[ATM_HEADER_LEN];
        atm_header_encode(&h, raw);
        atm_header_t back;
        atm_header_decode(raw, &back);
        CHECK(strcmp(back.name, h.name) == 0 && back.load == h.load &&
              back.exec == h.exec && back.len == h.len, "ATM header round trip");
        CHECK(raw[16] == 0x00 && raw[17] == 0x29 && raw[20] == 0x34 && raw[21] == 0x12,
              "ATM fields are little-endian");
        raw[5] = 0x0D;
        atm_header_decode(raw, &back);
        CHECK(strcmp(back.name, "SIXTE") == 0, "a CR ends the name: '%s'", back.name);
    }

    /* Lengths either side of a block boundary, both address modes. The
     * data is saved from where it reloads, which is all an ATM header can
     * describe; the end address seeds the flags byte (tape.c, record). */
    static const tcase_t cases[] = {
        { "ONE",       0x3000, 1,     0x3000, 0x3000, true,  0 },
        { "BLOCK",     0x3000, 256,   0x3000, 0xC2B2, true,  0 },
        { "BLOCKPLUS", 0x2900, 257,   0x2900, 0x2900, true,  0 },
        { "LONGER",    0x2A00, 1000,  0x2A00, 0x1234, false, 0x3400 },
        { "BIG",       0x2A00, 3000,  0x2A00, 0x2A00, true,  0 },
        { "ALMOST",    0x3000, 255,   0x3000, 0x3000, false, 0x0400 },
        { "UNALIGNED", 0x2987, 700,   0x2987, 0x2987, true,  0 },
        { "THIRTEENCHRS", 0x3000, 2,  0x3000, 0x3000, false, 0x3E00 },
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (run_case(&cases[i])) return 1;
    }

    /* ---- through the keyboard, the way a user drives it ------------- */

    /* A port that serves every request from one ATM held in memory, and
     * counts what it was asked for. */
    g.on_field = on_field;

    /* BASIC: SAVE, NEW, LOAD, RUN. */
    atom_copy(&g.m, &booted);
    keymatrix_init(&g.k);
    atm_len = 0;
    guest_type(&g, "10 P.\"TAPE OK\"'\n20 END\nSAVE \"PROG\"\n");
    atm_header_t h;
    atm_header_decode(atm, &h);
    CHECK(atm_len > ATM_HEADER_LEN && strcmp(h.name, "PROG") == 0,
          "SAVE wrote %zu bytes named '%s'", atm_len, h.name);
    /* BASIC saves from PAGE (#2900), to run from its warm start. */
    CHECK(h.load == 0x2900 && h.exec == 0xC2B2, "SAVE: load %04X exec %04X", h.load, h.exec);
    CHECK(strcmp(guest_row(&g.m, 5), ">") == 0, "SAVE returns to the prompt: '%s'",
          guest_row(&g.m, 5));
    guest_type(&g, "NEW\nLOAD \"PROG\"\nRUN\n");
    CHECK(strcmp(guest_row(&g.m, 8), "TAPE OK") == 0, "LOAD, RUN: '%s'", guest_row(&g.m, 8));
    CHECK(g.m.tape.served == 2, "served %u requests", (unsigned)g.m.tape.served);

    /* *RUN of machine code at its own address: write HI to the top left
     * of the screen and return to the MOS. */
    {
        static const uint8_t code[] = {
            0xA9, 0x08, 0x8D, 0x00, 0x80,     /* LDA #'H' : STA #8000 */
            0xA9, 0x09, 0x8D, 0x01, 0x80,     /* LDA #'I' : STA #8001 */
            0x60,                             /* RTS                  */
        };
        atm_header_t mc = { .name = "HI", .load = 0x2A00, .exec = 0x2A00,
                            .len = sizeof code };
        atm_header_encode(&mc, atm);
        memcpy(atm + ATM_HEADER_LEN, code, sizeof code);
        atm_len = ATM_HEADER_LEN + sizeof code;
        atom_copy(&g.m, &booted);
        keymatrix_init(&g.k);
        guest_type(&g, "*RUN \"HI\"\n");
        CHECK(strncmp(guest_row(&g.m, 0), "HI", 2) == 0, "*RUN: '%s'", guest_row(&g.m, 0));
        CHECK(strcmp(guest_row(&g.m, 3), ">") == 0, "*RUN returns: '%s'", guest_row(&g.m, 3));

        /* *LOAD with an address puts it there instead. */
        atom_copy(&g.m, &booted);
        keymatrix_init(&g.k);
        guest_type(&g, "*LOAD \"HI\" 3000\n");
        CHECK(memcmp(&g.m.ram[0x3000], code, sizeof code) == 0 && g.m.ram[0x2A00] == 0,
              "*LOAD at an address");
    }

    /* A name nobody has: the trap stands aside and the ROM asks for the
     * tape, as the real machine would. */
    atom_copy(&g.m, &booted);
    keymatrix_init(&g.k);
    guest_type(&g, "*LOAD \"NOSUCH\"\n");
    CHECK(g.m.tape.served == 0 && atom_tape_pending(&g.m) == NULL, "declined");
    CHECK(strcmp(guest_row(&g.m, 3), "PLAY TAPE") == 0, "declined: '%s'", guest_row(&g.m, 3));

    /* BREAK while a request is outstanding: the reset wins and the
     * request goes with the program that made it. */
    g.on_field = NULL;
    atom_copy(&g.m, &booted);
    keymatrix_init(&g.k);
    guest_type(&g, "*LOAD \"HI\"\n");
    CHECK(atom_tape_pending(&g.m) != NULL, "a request is outstanding");
    guest_chord(&g, PICOCALC_KEY_ALT, 'K');
    guest_fields(&g, 60);
    CHECK(atom_tape_pending(&g.m) == NULL, "BREAK cancels the request");
    CHECK(strcmp(guest_row(&g.m, 2), ">") == 0, "BREAK: '%s'", guest_row(&g.m, 2));

    /* A kernel that is not the stock one is left alone. */
    atom_copy(&g.m, &booted);
    keymatrix_init(&g.k);
    g.m.ram[TAPE_OSLOAD_PC + 6] ^= 0xFFu;       /* the ROM page, via ram[] */
    guest_type(&g, "*LOAD \"HI\"\n");
    CHECK(atom_tape_pending(&g.m) == NULL && g.m.tape.served == 0,
          "a different MOS should not be trapped");

    TEST_DONE();
}
