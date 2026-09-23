/* test_uef.c — tape phase 2 below the guest: gzip, the UEF waveform and
 * the cassette's timing (design.md §11.3).
 *
 * inflate is held to the system's gzip, which is the reference a UEF
 * archive was made with: every stream here is written by it, over data
 * chosen to force stored, fixed and dynamic blocks. The UEF walker and
 * the cassette are held to the spec's arithmetic: 1200 and 2400 Hz at
 * 300 baud, a byte as start, eight bits LSB first, stop.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atom.h"
#include "bus.h"
#include "cassette.h"
#include "inflate.h"
#include "test_util.h"
#include "uef.h"

/* ---- inflate against gzip ------------------------------------------------ */

typedef struct { const uint8_t *p; size_t n, at; } src_t;

static int get(void *ctx) {
    src_t *s = ctx;
    return s->at < s->n ? s->p[s->at++] : -1;
}

static uint8_t plain[200000], packed[220000], out[200000];

/* gzip `n` bytes of plain[] at `level`; the compressed length. */
static size_t gzip(size_t n, int level) {
    FILE *f = fopen("test_uef.in", "wb");
    if (!f) return 0;
    fwrite(plain, 1, n, f);
    fclose(f);
    char cmd[128];
    snprintf(cmd, sizeof cmd, "gzip -c -n -%d test_uef.in > test_uef.gz", level);
    if (system(cmd) != 0) return 0;
    f = fopen("test_uef.gz", "rb");
    if (!f) return 0;
    size_t len = fread(packed, 1, sizeof packed, f);
    fclose(f);
    return len;
}

static gz_status_t unpack(size_t len, size_t cap, size_t *got) {
    src_t s = { packed, len, 0 };
    return gunzip(get, &s, out, cap, got);
}

static uint32_t rng = 0x12345678u;
static uint8_t rnd(void) {
    rng ^= rng << 13; rng ^= rng >> 17; rng ^= rng << 5;
    return (uint8_t)rng;
}

static int test_inflate(void) {
    if (system("gzip --version > /dev/null 2>&1") != 0) {
        CHECK(0, "gzip is needed on the PATH: it is the reference for inflate");
        return 0;
    }

    /* Each shape forces a block type: random bytes do not compress, so
     * gzip stores them; a short string gets the fixed code; long text
     * gets a dynamic one; a run exercises copies that overlap. */
    struct { const char *what; size_t n; int kind; } cases[] = {
        { "empty",        0,      0 },
        { "short text",   40,     1 },
        { "long text",    150000, 1 },
        { "random",       100000, 2 },
        { "one long run", 70000,  3 },
        { "mixed",        180000, 4 },
    };
    const char *words = "10 PRINT \"HELLO ATOM\"; 20 GOTO 10 ";
    for (unsigned c = 0; c < sizeof cases / sizeof cases[0]; c++) {
        size_t n = cases[c].n;
        for (size_t i = 0; i < n; i++) {
            switch (cases[c].kind) {
            case 1: plain[i] = (uint8_t)words[(i * 7 + i / 13) % strlen(words)]; break;
            case 2: plain[i] = rnd(); break;
            case 3: plain[i] = 0xAA; break;
            default: plain[i] = (i / 4096) % 2 ? rnd() : (uint8_t)words[i % strlen(words)];
            }
        }
        for (int level = 1; level <= 9; level += 8) {
            size_t len = gzip(n, level);
            CHECK(len > 0, "%s: gzip failed", cases[c].what);
            size_t got = 0;
            gz_status_t st = unpack(len, sizeof out, &got);
            CHECK(st == GZ_OK, "%s -%d: %s", cases[c].what, level, gunzip_status_str(st));
            CHECK(got == n && memcmp(out, plain, n) == 0,
                  "%s -%d: %zu bytes back, %zu expected, or they differ",
                  cases[c].what, level, got, n);
        }
    }

    /* The refusals: each leaves a status that says which. */
    size_t n = 5000;
    for (size_t i = 0; i < n; i++) plain[i] = (uint8_t)words[i % strlen(words)];
    size_t len = gzip(n, 9);
    size_t got;
    CHECK(unpack(len, n - 1, &got) == GZ_TOO_BIG, "one byte short of room is refused");
    CHECK(unpack(len / 2, sizeof out, &got) == GZ_TRUNCATED, "half a stream is truncated");
    packed[len - 5] ^= 0x01u;                     /* in the stored CRC */
    CHECK(unpack(len, sizeof out, &got) == GZ_BAD_CHECK, "a CRC that disagrees is refused");
    packed[len - 5] ^= 0x01u;
    packed[0] = 'U';
    CHECK(unpack(len, sizeof out, &got) == GZ_NOT_GZIP, "no magic, not gzip");

    remove("test_uef.in");
    remove("test_uef.gz");
    return 0;
}

/* ---- the UEF waveform ---------------------------------------------------- */

static uint8_t img[4096];
static size_t  img_len;

static void uef_begin(void) {
    memcpy(img, "UEF File!\0\x0a\x00", UEF_HEADER_LEN);
    img_len = UEF_HEADER_LEN;
}

static void chunk(uint16_t id, const uint8_t *body, uint32_t n) {
    uint8_t *h = img + img_len;
    h[0] = (uint8_t)id; h[1] = (uint8_t)(id >> 8);
    h[2] = (uint8_t)n; h[3] = (uint8_t)(n >> 8); h[4] = (uint8_t)(n >> 16); h[5] = (uint8_t)(n >> 24);
    memcpy(h + 6, body, n);
    img_len += 6u + n;
}

static void chunk16(uint16_t id, uint16_t v) {
    uint8_t b[2] = { (uint8_t)v, (uint8_t)(v >> 8) };
    chunk(id, b, 2);
}

/* What the tape should be: half-cycles of `units` with an edge, or a
 * gap. */
typedef struct { uint32_t units; bool edge; } piece_t;
static piece_t want[4096];
static unsigned n_want;

static void expect(unsigned halves, uint32_t units) {
    for (unsigned i = 0; i < halves; i++) want[n_want++] = (piece_t){ units, true };
}

/* A byte at `cycles` base cycles per bit: a 0 is that many at 1200 Hz,
 * a 1 twice as many at 2400 (§11.3, #FC9C). */
static void expect_byte(uint8_t v, unsigned cycles) {
    unsigned frame = ((unsigned)v << 1) | 0x200u;
    for (int b = 0; b < 10; b++) {
        if ((frame >> b) & 1u) expect(4u * cycles, 1);
        else                   expect(2u * cycles, 2);
    }
}

static int test_waveform(void) {
    uef_begin();
    uint8_t meta[] = "made by test_uef";
    chunk(0x0000, meta, sizeof meta);          /* origin: skipped */
    chunk16(0x0110, 10);                        /* 10 cycles of 2400 Hz */
    uint8_t data[] = { 0x55, 0x00 };
    chunk(0x0100, data, 2);
    chunk16(0x0112, 100);                       /* 100/2400 s */
    chunk16(0x0117, 1200);                      /* a BBC's baud */
    uint8_t ff = 0xFF;
    chunk(0x0100, &ff, 1);
    uint8_t dummy[4] = { 3, 0, 2, 0 };
    chunk(0x0111, dummy, 4);
    uint8_t fmt[] = { 7, 'E', 2, 0x41 };        /* 7E2, one byte */
    chunk(0x0104, fmt, sizeof fmt);
    uint8_t sec[] = { 3, 0, 0, 'P', 'W', 0xA0 };   /* 1, 0, 1 */
    chunk(0x0114, sec, sizeof sec);
    uint8_t neg[] = { 8, 'N', (uint8_t)-1, 0x00, 0xFF };  /* 8N1 + a short wave */
    chunk(0x0104, neg, sizeof neg);

    n_want = 0;
    expect(20, 1);
    expect_byte(0x55, 4);                       /* 300 baud: no &0117 yet */
    expect_byte(0x00, 4);
    want[n_want++] = (piece_t){ 200, false };
    expect_byte(0xFF, 1);
    expect(6, 1); expect_byte(0xAA, 1); expect(4, 1);
    /* 7E2 of #41: start, 1000001, parity 0 (two ones), stop, stop. */
    unsigned bits7e2[] = { 0, 1, 0, 0, 0, 0, 0, 1, 0, 1, 1 };
    for (unsigned i = 0; i < 11; i++) {
        if (bits7e2[i]) expect(4, 1); else expect(2, 2);
    }
    /* Security cycles: the first a single pulse, then whole cycles. */
    expect(1, 1); expect(2, 2); expect(2, 1);
    /* A stop count of -1: one stop bit, then one cycle at 2400 Hz, after
     * every byte — MakeUEF writes the Atom's inter-byte cycle this way. */
    expect_byte(0x00, 1); expect(2, 1);
    expect_byte(0xFF, 1); expect(2, 1);

    uef_t u;
    CHECK(uef_open(&u, img, img_len), "a UEF header opens");
    unsigned i = 0;
    uint32_t units;
    bool edge;
    while (uef_next(&u, &units, &edge)) {
        if (i < n_want) {
            CHECK(want[i].units == units && want[i].edge == edge,
                  "piece %u: %u units %s, expected %u %s", i, units,
                  edge ? "edge" : "gap", want[i].units, want[i].edge ? "edge" : "gap");
        }
        i++;
    }
    CHECK(i == n_want, "%u pieces played, %u expected", i, n_want);
    CHECK(uef_percent(&u) == 100, "the end is 100%%, not %u", uef_percent(&u));

    uef_rewind(&u);
    CHECK(uef_next(&u, &units, &edge) && units == 1 && edge, "rewind goes back to the tone");

    CHECK(!uef_open(&u, (const uint8_t *)"UEF Filf!\0\0\0", 12), "a wrong magic is refused");
    CHECK(!uef_open(&u, img, 5), "a stub is refused");

    /* A truncated chunk plays as far as the file goes, and no further. */
    uef_begin();
    chunk(0x0100, data, 2);
    img_len -= 1;
    CHECK(uef_open(&u, img, img_len), "opens");
    i = 0;
    while (uef_next(&u, &units, &edge)) i++;
    CHECK(i == 8u + 4u * 16u + 4u * 8u + 16u, "one byte of two: %u pieces", i);
    return 0;
}

/* ---- the cassette in guest cycles -------------------------------------- */

static int test_timing(void) {
    uef_begin();
    chunk16(0x0110, 600);                       /* 1200 half-cycles, a quarter second */
    uint8_t data[] = { 0x00, 0xFF, 0x3C };
    chunk(0x0100, data, 3);
    chunk16(0x0112, 480);                       /* 0.2 s */
    chunk16(0x0110, 24);

    cassette_t c;
    cassette_init(&c);
    CHECK(cassette_insert(&c, img, img_len), "inserts");
    CHECK(!c.playing && cassette_input(&c, 100000) == false, "inserted is stopped");

    /* Every edge's cycle, reading the input on every cycle. */
    static uint64_t edges[4000];
    unsigned n = 0;
    uint64_t start = 5000, t = start;
    cassette_play(&c, start, true);
    bool level = cassette_input(&c, t);
    uint64_t stop_at = start + 300000, stopped_for = 12345;
    uint64_t units_total = 0;
    bool stopped = false;
    while (c.playing || stopped) {
        t++;
        if (!stopped && t == stop_at) { cassette_play(&c, t, false); stopped = true; }
        if (stopped && t == stop_at + stopped_for) { cassette_play(&c, t, true); stopped = false; }
        bool l = cassette_input(&c, t);
        if (l != level && n < 4000) edges[n++] = t;
        level = l;
        CHECK(t < 10000000u, "the tape ends");
        if (t >= 10000000u) break;
    }
    /* 1224 tone halves, 3 bytes (8 and 16 halves a bit), 48 tone halves. */
    unsigned halves_0 = 8, halves_1 = 16;
    unsigned bytes_halves = 3u * (halves_0 + halves_1)       /* start + stop */
                          + 8u * halves_0 + 8u * halves_1 + 4u * halves_0 + 4u * halves_1;
    CHECK(n == 1200u + bytes_halves + 48u, "%u edges", n);
    units_total = 1200u + 3u * (8u * 2u + 16u) + 8u * 16u + 8u * 16u + 4u * 16u + 4u * 16u
                + 960u + 48u;

    /* Every half-cycle is 208 or 209 cycles at 2400 Hz and 416 or 417 at
     * 1200 — 1,000,000 / 4800 per unit, the remainder carried — except
     * the one that spans the pause, which is longer by the pause, and
     * the one after the gap. */
    unsigned odd = 0;
    for (unsigned i = 1; i < n; i++) {
        uint64_t d = edges[i] - edges[i - 1];
        bool ok = d == 208 || d == 209 || d == 416 || d == 417;
        if (!ok) odd++;
    }
    CHECK(odd == 2, "%u half-cycles off the grid; the pause and the gap are the two", odd);

    /* The last edge lands where the arithmetic says, to the cycle, pause
     * included: nothing drifts. The first edge ends the first half. */
    uint64_t end = start + stopped_for + (units_total * 1000000u) / 4800u;
    CHECK(edges[n - 1] == end, "last edge at %llu, expected %llu",
          (unsigned long long)edges[n - 1], (unsigned long long)end);
    CHECK(c.ended && cassette_percent(&c) == 100, "played to the end");

    /* A tape at its end does not play until it is rewound. */
    cassette_play(&c, t, true);
    CHECK(!c.playing, "an ended tape stays stopped");
    cassette_rewind(&c, t);
    cassette_play(&c, t, true);
    CHECK(c.playing && cassette_percent(&c) < 100, "rewound, it plays");

    /* A restored snapshot moves the clock back; the tape carries on from
     * where it is, on the new clock, rather than waiting for the old one
     * to come round again. */
    uint64_t now = t + 100000u;
    cassette_input(&c, now);
    uint64_t left = c.edge - now;
    unsigned before = c.edges;
    cassette_retime(&c, now, 5000u);
    CHECK(c.edge == 5000u + left, "the next edge is as far ahead as it was");
    cassette_input(&c, 5000u + left);
    CHECK(c.edges == before + 1u, "and arrives on the new clock");
    cassette_t q;
    cassette_init(&q);
    cassette_retime(&q, 0, 416u * 1000u + 100u);
    CHECK(cassette_ref_2400(&q, 416u * 1000u + 100u) && !cassette_ref_2400(&q, 416u * 1001u + 210u),
          "the reference's phase after a restore is the cycle count's");

    /* The 2.4 kHz reference: 416 cycles a period, half high, and exact
     * over a long run whatever the read pattern (§16's constant). */
    cassette_t r;
    cassette_init(&r);
    unsigned high = 0, rises = 0;
    bool prev = cassette_ref_2400(&r, 0);
    for (uint64_t k = 1; k <= 416u * 1000u; k += 1u + (k % 7u)) {
        bool v = cassette_ref_2400(&r, k);
        if (v && !prev) rises++;
        prev = v;
    }
    CHECK(rises == 1000 || rises == 999, "%u rising edges in 1000 periods", rises);
    cassette_init(&r);
    for (uint64_t k = 0; k < 416u * 10u; k++) high += cassette_ref_2400(&r, k);
    CHECK(high == 208u * 10u, "high for half of each period: %u of %u", high, 4160u);
    /* Across a 32-bit wrap of the cycle count, read as the MOS reads it,
     * the period stays 416: the reference is kept in 32 bits. */
    uint64_t k0 = 0x100000000ull - 20000u, last_rise = 0;
    unsigned bad = 0;
    prev = cassette_ref_2400(&r, k0);
    for (uint64_t k = k0 + 1; k < k0 + 40000u; k++) {
        bool v = cassette_ref_2400(&r, k);
        if (v && !prev) {
            if (last_rise && k - last_rise != 416u) bad++;
            last_rise = k;
        }
        prev = v;
    }
    CHECK(bad == 0, "%u periods not 416 cycles across the wrap", bad);
    return 0;
}

/* ---- through the bus --------------------------------------------------- */

static int test_port(void) {
    static atom_t m;
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&m, &cfg);
    bus_write(&m, 0xB003, 0x8Au);    /* the MOS's control word: C high nibble in */

    uef_begin();
    chunk16(0x0110, 100);
    CHECK(atom_cassette_insert(&m, img, img_len), "inserts");
    atom_cassette_play(&m, true);

    unsigned changes = 0, ref_changes = 0;
    uint8_t last = bus_read(&m, 0xB002);
    for (int i = 0; i < 20000; i++) {
        m.cpu.cycles += 10;
        uint8_t v = bus_read(&m, 0xB002);
        if ((v ^ last) & 0x20u) changes++;
        if ((v ^ last) & 0x10u) ref_changes++;
        last = v;
    }
    CHECK(changes == 200, "port C bit 5 follows the tape: %u changes, 200 expected", changes);
    CHECK(ref_changes >= 960 && ref_changes <= 962, "bit 4 is 2.4 kHz: %u changes in 0.2 s",
          ref_changes);
    CHECK(!atom_cassette_playing(&m), "the tone has run out");

    atom_cassette_eject(&m);
    CHECK(!m.cas.loaded, "ejected");
    return 0;
}

int main(void) {
    if (test_inflate()) return 1;
    if (test_waveform()) return 1;
    if (test_timing()) return 1;
    if (test_port()) return 1;
    TEST_DONE();
}
