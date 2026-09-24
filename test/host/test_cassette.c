/* test_cassette.c — tape phase 2 with the real MOS (design.md §11.3).
 *
 * The reference is the kernel ROM at both ends. Its own SAVE runs with
 * no trap and no hook, timing every bit against port C bit 4, and what
 * it drives onto port C bits 0-1 is recorded here, cycle by cycle, as
 * the signal a recorder would see. That waveform is decoded into a UEF
 * the way an archivist's tools would, and the ROM's own LOAD then reads
 * it back off port C bit 5 through the cassette. The program must come
 * back byte for byte and run.
 *
 * Then the M8 case in miniature (§17): a headerless block read by a
 * loader of its own, which never calls OSLOAD and so is out of phase
 * 1's reach, loads from a UEF.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "guest.h"
#include "test_util.h"
#include "uef.h"

static guest_t g;

/* ---- the recorder: port C bits 0-1 as a waveform ----------------------- */

/* Bit 1 gates the 2.4 kHz reference onto the output; with it clear the
 * output is bit 0 as it stands (§2.3, #FC92 and #FC9E). */
static bool out_level(const atom_t *m, uint8_t out_c, uint64_t t) {
    if (!(out_c & 0x01u)) return false;
    if (!(out_c & 0x02u)) return true;
    uint32_t d = (uint32_t)t - m->cas.hz_ref;
    return d % ATOM_CASSETTE_REF_CYCLES < ATOM_CASSETTE_REF_CYCLES / 2u;
}

static uint64_t edges[400000];
static unsigned n_edges;
static bool     rec_level;
static uint64_t rec_end;        /* when the recording stopped */

/* One instruction at a time, so every change on the output is stamped
 * to the cycle. The same field split as atom_run_field. */
static void run_recorded(atom_t *m, uint32_t cycles) {
    m->budget += (int32_t)cycles;
    while (m->budget > 0) {
        uint64_t t0 = m->cpu.cycles;
        uint8_t oc = m->ppi.out_c;
        m->budget -= (int32_t)atom_run(m, 1);
        for (uint64_t t = t0; t < m->cpu.cycles; t++) {
            bool l = out_level(m, oc, t);
            if (l != rec_level && n_edges < sizeof edges / sizeof edges[0]) edges[n_edges++] = t;
            rec_level = l;
        }
    }
}

static void fields_recorded(int n) {
    atom_t *m = &g.m;
    for (int i = 0; i < n; i++) {
        keymatrix_field(&g.k, m);
        uint32_t fly = ATOM_FLYBACK_CYCLES;
        run_recorded(m, ATOM_CYCLES_PER_FIELD - fly);
        atom_field_sync(m, true);
        run_recorded(m, fly);
        atom_field_sync(m, false);
        int16_t discard[ATOM_AUDIO_BUF_LEN];
        (void)atom_audio_drain(m, discard, ATOM_AUDIO_BUF_LEN);
    }
    rec_end = m->cpu.cycles;
}

/* ---- the waveform as a UEF ---------------------------------------------- */

static uint8_t uef[70000];
static size_t  uef_len;
static uint8_t bytes[4096];     /* every byte decoded, in order */
static unsigned n_bytes;

static void put_chunk(uint16_t id, const uint8_t *b, uint32_t n) {
    uint8_t *h = uef + uef_len;
    h[0] = (uint8_t)id; h[1] = (uint8_t)(id >> 8);
    h[2] = (uint8_t)n; h[3] = (uint8_t)(n >> 8); h[4] = 0; h[5] = 0;
    memcpy(h + 6, b, n);
    uef_len += 6u + n;
}

static void put16(uint16_t id, uint32_t v) {
    if (v > 0xFFFFu) v = 0xFFFFu;
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    put_chunk(id, b, 2);
}

static uint8_t run[4096];
static unsigned n_run, tone_halves;

static void flush(void) {
    if (tone_halves) put16(0x0110, (tone_halves + 1u) / 2u);
    tone_halves = 0;
    if (n_run) put_chunk(0x0100, run, n_run);
    n_run = 0;
}

/* Half-cycles classified by length: 2400 Hz ~208, 1200 Hz ~417,
 * anything else a gap in the signal. */
enum { H_SHORT, H_LONG, H_GAP };
static int kind(uint64_t d) {
    if (d >= 150 && d <= 280) return H_SHORT;
    if (d >= 330 && d <= 500) return H_LONG;
    return H_GAP;
}

/* Returns the number of framing errors. A byte is eight long halves
 * (the start bit), then eight bits and a stop bit, each eight long
 * halves for a 0 or sixteen short for a 1 (§11.3). */
static unsigned decode(void) {
    memcpy(uef, "UEF File!\0\x0a\x00", UEF_HEADER_LEN);
    uef_len = UEF_HEADER_LEN;
    put16(0x0117, 300);
    n_bytes = n_run = tone_halves = 0;
    unsigned errors = 0;

    unsigned i = 1;
    while (i < n_edges) {
        uint64_t d = edges[i] - edges[i - 1];
        int k = kind(d);
        if (k == H_SHORT) {
            if (n_run) { put_chunk(0x0100, run, n_run); n_run = 0; }
            tone_halves++;
            i++;
            continue;
        }
        if (k == H_GAP) {
            flush();
            uint32_t units = (uint32_t)(d * 2400u / 1000000u);
            if (units) put16(0x0112, units);
            i++;
            continue;
        }
        /* A start bit. */
        if (tone_halves) { put16(0x0110, (tone_halves + 1u) / 2u); tone_halves = 0; }
        unsigned frame = 0;
        bool bad = false;
        for (unsigned bit = 0; bit < 10 && !bad; bit++) {
            if (i >= n_edges) { bad = true; break; }
            int first = kind(edges[i] - edges[i - 1]);
            unsigned halves = first == H_LONG ? 8u : 16u;
            if (first == H_GAP) { bad = true; break; }
            for (unsigned h = 0; h < halves; h++, i++) {
                if (i >= n_edges || kind(edges[i] - edges[i - 1]) != first) { bad = true; break; }
            }
            if (first == H_SHORT) frame |= 1u << bit;
        }
        if (bad || (frame & 1u) || !(frame & 0x200u)) {
            errors++;
            continue;
        }
        uint8_t v = (uint8_t)(frame >> 1);
        if (n_bytes < sizeof bytes) bytes[n_bytes++] = v;
        if (n_run < sizeof run) run[n_run++] = v;
    }
    flush();
    /* The silence after the last edge, which no edge closes. */
    if (n_edges && rec_end > edges[n_edges - 1]) {
        uint32_t units = (uint32_t)((rec_end - edges[n_edges - 1]) * 2400u / 1000000u);
        if (units) put16(0x0112, units);
    }
    return errors;
}

/* ---- the tests ---------------------------------------------------------- */

static bool screen_has(const atom_t *m, const char *text) {
    for (int r = 0; r < 16; r++) {
        if (strstr(guest_row(m, r), text)) return true;
    }
    return false;
}

static void dump_screen(const atom_t *m) {
    for (int r = 0; r < 16; r++) fprintf(stderr, "    |%s\n", guest_row(m, r));
}

/* Where the ROM put the data and how much, from the last block header
 * decoded: the eight bytes after "****", the name and CR, written from
 * #D2 down to #CB (#FB56). */
static bool block_header(const char *name, uint16_t *load, unsigned *len) {
    size_t nl = strlen(name);
    for (unsigned i = 0; i + 4u + nl + 1u + 8u <= n_bytes; i++) {
        if (memcmp(bytes + i, "****", 4) || memcmp(bytes + i + 4, name, nl) ||
            bytes[i + 4 + nl] != 0x0D) continue;
        const uint8_t *h = bytes + i + 4 + nl + 1;
        *load = (uint16_t)((h[6] << 8) | h[7]);
        *len = h[3] + 1u;
        return true;
    }
    return false;
}

/* Type `lines` at a machine with no trap, then `save`, with the ROM
 * itself driving the port, and leave the recording decoded into uef[]. */
static int record_program(const char *const *lines, const char *save) {
    guest_boot(&g);
    g.m.cfg.tape_traps = false;
    for (; *lines; lines++) guest_type(&g, *lines);
    guest_type(&g, save);
    CHECK(screen_has(&g.m, "RECORD TAPE"), "the MOS asks for the recorder");

    n_edges = 0;
    rec_level = false;
    keymatrix_event(&g.k, KEY_EV_PRESSED, 0x0Au);
    keymatrix_event(&g.k, KEY_EV_RELEASED, 0x0Au);
    fields_recorded(900);                       /* fifteen seconds */
    printf("  recorded     : %.*s, %u edges\n", (int)strlen(save) - 1, save, n_edges);
    CHECK(n_edges > 5000, "the ROM wrote a signal: %u edges", n_edges);
    CHECK(guest_cursor(&g.m) >= 0, "SAVE returned to the prompt");

    unsigned errors = decode();
    printf("  decoded      : %u bytes, %u framing errors, UEF %zu bytes\n",
           n_bytes, errors, uef_len);
    CHECK(errors == 0, "the ROM's writer frames every byte as §11.3 says: %u errors", errors);
    return 0;
}

static int test_round_trip(void) {
    static const char *const prog[] = { "10 PRINT \"SIGNAL OK\"\n", "20 END\n", NULL };
    if (record_program(prog, "SAVE \"PROG\"\n")) return 1;
    uint16_t load = 0;
    unsigned len = 0;
    CHECK(block_header("PROG", &load, &len), "a PROG block is on the tape");
    CHECK(load == 0x2900u, "saved from #2900, not #%04X", load);
    static uint8_t saved[256];
    memcpy(saved, &g.m.ram[load], len);

    /* The loading machine: traps on, as shipped. With a UEF in the deck
     * the trap stands aside and the ROM reads the signal. */
    guest_boot(&g);
    char first[14];
    CHECK(uef_first_name(uef, uef_len, first) && strcmp(first, "PROG") == 0,
          "the menu can say what to LOAD");
    CHECK(atom_cassette_insert(&g.m, uef, uef_len), "the recording is a UEF");
    CHECK(!g.m.cas.playing, "a tape goes in stopped");
    guest_type(&g, "LOAD \"PROG\"\n");
    CHECK(atom_tape_pending(&g.m) == NULL, "the trap stands aside for a UEF");
    CHECK(screen_has(&g.m, "PLAY TAPE"), "the MOS asks for the tape");
    CHECK(!g.m.cas.playing, "and the deck waits for the key that answers it");
    guest_tap(&g, 0x0Au);
    CHECK(g.m.cas.playing, "the key starts the deck (§11.3's cues)");
    for (int i = 0; i < 900 && g.m.cas.playing; i++) guest_fields(&g, 1);
    guest_fields(&g, 30);
    CHECK(!g.m.cas.ended, "the load stopped the deck, not the end of the tape");
    CHECK(g.m.tape.served == 0, "phase 1 served nothing: the signal did it");
    CHECK(memcmp(&g.m.ram[load], saved, len) == 0,
          "the %u bytes at #%04X came back through the signal", len, load);

    guest_type(&g, "RUN\n");
    CHECK(screen_has(&g.m, "SIGNAL OK"), "and they run");
    if (test_failures) dump_screen(&g.m);

    /* A *RUN's program may read on from the tape itself, so the deck
     * keeps playing after it: machine code that prints '!' and returns. */
    static const char *const mc[] = {
        "?#3C00=#A9;?#3C01=#21;?#3C02=#20\n",   /* the input line is 64 */
        "?#3C03=#F4;?#3C04=#FF;?#3C05=#60\n", NULL,
    };
    if (record_program(mc, "*SAVE \"MC\" 3C00 3C06 3C00\n")) return 1;
    guest_boot(&g);
    atom_cassette_insert(&g.m, uef, uef_len);
    guest_type(&g, "*RUN \"MC\"\n");
    guest_tap(&g, 0x0Au);
    for (int i = 0; i < 900 && g.m.ram[0x3C05] != 0x60; i++) guest_fields(&g, 1);
    guest_fields(&g, 5);
    CHECK(g.m.cas.playing, "a *RUN leaves the deck playing, into the gap after the file");
    guest_fields(&g, 30);
    CHECK(screen_has(&g.m, "!"), "*RUN loads it and runs it");
    if (test_failures) dump_screen(&g.m);
    return 0;
}

/* A loader of its own at #3C00: 64 bytes straight off the tape through
 * the ROM's byte reader (#FBEE), with no name, no header, no OSLOAD. */
static const uint8_t loader[] = {
    0xA0, 0x00,             /* LDY #0         */
    0x20, 0xEE, 0xFB,       /* JSR #FBEE      */
    0x99, 0x00, 0x38,       /* STA #3800,Y    */
    0xC8,                   /* INY            */
    0xC0, 0x40,             /* CPY #64        */
    0xD0, 0xF5,             /* BNE #3C02      */
    0x60,                   /* RTS            */
};

static int test_headerless(void) {
    uint8_t data[64];
    uint32_t r = 0xC0FFEEu;
    for (unsigned i = 0; i < sizeof data; i++) {
        r = r * 1103515245u + 12345u;
        data[i] = (uint8_t)(r >> 16);
    }
    /* An image as archives carry them, without &0117: 300 baud is the
     * default here (uef.h). */
    memcpy(uef, "UEF File!\0\x0a\x00", UEF_HEADER_LEN);
    uef_len = UEF_HEADER_LEN;
    put16(0x0110, 4800);
    put_chunk(0x0100, data, sizeof data);
    put16(0x0112, 2400);

    guest_boot(&g);
    memcpy(&g.m.ram[0x3C00], loader, sizeof loader);
    guest_type(&g, "LINK #3C00");
    CHECK(atom_cassette_insert(&g.m, uef, uef_len), "inserts");
    atom_cassette_play(&g.m, true);
    guest_type(&g, "\n");
    for (int i = 0; i < 600 && g.m.cas.playing; i++) guest_fields(&g, 1);
    guest_fields(&g, 30);
    CHECK(memcmp(&g.m.ram[0x3800], data, sizeof data) == 0,
          "a headerless block loads through a loader phase 1 never sees");
    CHECK(guest_cursor(&g.m) >= 0, "and the loader returns to BASIC");
    return 0;
}

/* The tape the hardware run uses (§17 M8), built and loaded here first
 * exactly as a user drives it: LOAD "LDR" reads a BASIC program off the
 * signal, and RUN pokes a loader of its own into #3C00 that reads the
 * rest of the tape, a headerless block, once the deck is played again.
 * Written out as m8-two-part.uef beside the test for the card. */
static int test_two_part(void) {
    static const char *const prog[] = {
        "10 !#3C00=#EE2000A0;!#3C04=#380099FB\n",
        "20 !#3C08=#D040C0C8;?#3C0C=#F5;?#3C0D=#60\n",
        "30 LINK #3C00\n",
        "40 S=0;FOR I=0 TO 63;S=S+?(#3800+I);NEXT I\n",
        "50 PRINT \"LOADED \"S\n",
        "60 END\n",
        NULL,
    };
    if (record_program(prog, "SAVE \"LDR\"\n")) return 1;
    CHECK(memcmp(&g.m.ram[0x3C00], loader, sizeof loader) != 0, "the loader is not in RAM yet");

    uint8_t data[64];
    unsigned sum = 0;
    for (unsigned i = 0; i < sizeof data; i++) {
        data[i] = (uint8_t)(i * 37u + 11u);
        sum += data[i];
    }
    /* Ten seconds of leader, so the user has time to type RUN. */
    put16(0x0110, 24000);
    put_chunk(0x0100, data, sizeof data);
    put16(0x0112, 4800);

    FILE *f = fopen("m8-two-part.uef", "wb");
    CHECK(f && fwrite(uef, 1, uef_len, f) == uef_len, "writes m8-two-part.uef");
    if (f) fclose(f);

    guest_boot(&g);
    CHECK(atom_cassette_insert(&g.m, uef, uef_len), "inserts");
    guest_type(&g, "LOAD \"LDR\"\n");
    guest_tap(&g, 0x0Au);
    /* Until the MOS is back at the prompt with the program in. */
    for (int i = 0; i < 1800; i++) {
        guest_fields(&g, 1);
        const char *row = guest_row(&g.m, 4);
        if (row[0] == '>' && row[1] == 0) break;
    }
    CHECK(!g.m.cas.playing, "LOAD's end stopped the deck");
    /* The loader reads the tape without OSLOAD, so it gets no cue: the
     * deck is played by hand, as on the real machine. */
    guest_type(&g, "RUN\n");
    atom_cassette_play(&g.m, true);
    for (int i = 0; i < 1800 && g.m.cas.playing; i++) guest_fields(&g, 1);
    guest_fields(&g, 60);
    char want[32];
    snprintf(want, sizeof want, "LOADED %u", sum);
    bool found = false;
    for (int r = 0; r < 16 && !found; r++) {
        const char *row = guest_row(&g.m, r);
        if (strncmp(row, "LOADED", 6) == 0) {
            unsigned v = 0;
            found = sscanf(row + 6, "%u", &v) == 1 && v == sum;
        }
    }
    CHECK(found, "RUN reads part 2 with its own loader and prints %s", want);
    CHECK(memcmp(&g.m.ram[0x3800], data, sizeof data) == 0, "part 2 is at #3800");
    CHECK(g.m.tape.served == 0, "phase 1 served nothing");
    if (test_failures) dump_screen(&g.m);
    return 0;
}

int main(void) {
    const char *dir;
    if (!guest_find_roms(&dir)) {
        printf("skipped: no kernel and BASIC images in %s\n", dir);
        return TEST_SKIP_CODE;
    }
    if (test_round_trip()) return 1;
    if (test_headerless()) return 1;
    if (test_two_part()) return 1;
    TEST_DONE();
}
