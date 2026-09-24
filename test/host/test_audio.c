/* test_audio.c — the loudspeaker integrator (design.md §9.3, §17 M5).
 *
 * §17 asks for the integrator to be verified against a known frequency.
 * The frequency comes from executing a guest loop whose half period is
 * counted by hand, and the samples are checked two ways: against an
 * independent box-filter model built from the edges the guest actually
 * made, and by measuring the pitch of the output.
 */

#include <math.h>
#include <string.h>

#include "atom.h"
#include "bus.h"
#include "test_util.h"

static atom_t g_machine;

/* #1000  LDA #8A ; STA #B003     8255 mode set: port C low is output
 * #1005  LDA #B002 ; EOR #04 ; STA #B002    toggle the speaker (4+2+4)
 * #100D  LDX #N                  (2)
 * #100F  DEX ; BNE #100F         (5N - 1)
 * #1012  JMP #1005               (3)
 * Half period: 12 + 5N - 1 + 3 = 14 + 5N cycles. */
#define LOOP_N     97u
#define HALF       (14u + 5u * LOOP_N)                  /* 499 cycles */
static const uint8_t toggle[] = {
    0xA9, 0x8A,  0x8D, 0x03, 0xB0,
    0xAD, 0x02, 0xB0,  0x49, 0x04,  0x8D, 0x02, 0xB0,
    0xA2, LOOP_N,
    0xCA,  0xD0, 0xFD,
    0x4C, 0x05, 0x10,
};

/* #1000  LDA #8A ; STA #B003 ; JMP #1005   configure, then idle */
static const uint8_t idle[] = {
    0xA9, 0x8A,  0x8D, 0x03, 0xB0,  0x4C, 0x05, 0x10,
};

static atom_t *machine(const uint8_t *code, size_t len) {
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&g_machine, &cfg);
    for (size_t i = 0; i < len; i++)
        bus_write(&g_machine, (uint16_t)(0x1000u + i), code[i]);
    g_machine.cpu.pc = 0x1000;
    return &g_machine;
}

#define MAX_SAMPLES 40000u
#define MAX_EDGES   2000u

static int16_t  samples[MAX_SAMPLES];
static uint64_t edges[MAX_EDGES];   /* cycle each instruction that moved PC2 began */

int main(void) {
    /* ---- the rate is kept as an exact fraction (§9.2) ----------------- */
    {
        atom_t *m = machine(idle, sizeof(idle));
        CHECK(m->beeper.num == 2048u && m->beeper.den == 75u,
              "150 MHz / 4096 should be 2048/75 cycles a sample, got %u/%u",
              m->beeper.num, m->beeper.den);

        /* One guest second is 36,621.09375 samples, not a truncated
         * 36,621: over 75 s the count comes out whole, and at any point it
         * is exactly the samples that have ended by the last cycle run. */
        uint64_t t0 = m->beeper.start;
        size_t total = 0;
        for (unsigned f = 0; f < 75u * 60u; f++) {
            atom_run(m, ATOM_CPU_HZ / 60u);
            int16_t junk[ATOM_AUDIO_BUF_LEN];
            size_t n;
            while ((n = atom_audio_drain(m, junk, ATOM_AUDIO_BUF_LEN)) > 0) total += n;
        }
        uint64_t want = ((m->cpu.cycles - t0) * 75u) / 2048u;
        CHECK(total == want, "%llu cycles should make %llu samples, made %zu",
              (unsigned long long)(m->cpu.cycles - t0), (unsigned long long)want, total);
        CHECK(m->beeper.overflow == 0, "overflow %u", m->beeper.overflow);
    }

    /* ---- every sample is the box filter of the guest's own edges ------ *
     * Step one instruction at a time, record when PC2 moved, and rebuild
     * each sample independently in double precision. */
    {
        atom_t *m = machine(toggle, sizeof(toggle));
        m->beeper.dc_block = false;
        uint64_t t0 = m->beeper.start;
        bool level0 = m->beeper.level;

        unsigned ne = 0;
        size_t ns = 0;
        bool was = atom_speaker(m);
        while (m->cpu.cycles - t0 < 200000u) {
            uint64_t at = m->cpu.cycles;
            atom_run(m, 1);
            if (atom_speaker(m) != was) {
                was = !was;
                if (ne < MAX_EDGES) edges[ne++] = at;
            }
            ns += atom_audio_drain(m, samples + ns, MAX_SAMPLES - ns);
        }
        CHECK(ne > 100, "the loop should toggle the speaker, saw %u edges", ne);

        /* The hand count, checked against execution. */
        for (unsigned i = 1; i < ne; i++) {
            CHECK(edges[i] - edges[i - 1] == HALF,
                  "edge %u: half period %llu, counted %u", i,
                  (unsigned long long)(edges[i] - edges[i - 1]), HALF);
        }

        const double T = 2048.0 / 75.0;
        unsigned e = 0;
        bool lvl = level0;
        int worst = 0;
        for (size_t k = 0; k < ns; k++) {
            double a = (double)t0 + T * (double)k, b = a + T, hi = 0, t = a;
            while (e < ne && (double)edges[e] < b) {
                double x = (double)edges[e];
                if (x > t) { if (lvl) hi += x - t; t = x; }
                lvl = !lvl;
                e++;
            }
            if (lvl) hi += b - t;
            int want = (int)lround(hi / T * BEEPER_FULL_SCALE);
            int d = abs(want - samples[k]);
            if (d > worst) worst = d;
        }
        CHECK(worst <= 1, "samples should match the box filter to 1 LSB, worst %d", worst);

        /* Not point sampling: at 998 cycles a period against 27.31 a
         * sample, some samples straddle an edge and are fractional. */
        unsigned fractional = 0;
        for (size_t k = 0; k < ns; k++)
            if (samples[k] != 0 && samples[k] != BEEPER_FULL_SCALE) fractional++;
        CHECK(fractional > ne / 2u, "edges should give fractional samples, %u of %zu",
              fractional, ns);
    }

    /* ---- a known frequency, measured off the output (§17 M5) ---------- *
     * One guest second through the real field loop, draining per field as
     * the port does, with the DC blocker in. */
    {
        atom_t *m = machine(toggle, sizeof(toggle));
        size_t ns = 0;
        for (unsigned f = 0; f < ATOM_FIELD_HZ; f++) {
            atom_run_field(m);
            ns += atom_audio_drain(m, samples + ns, MAX_SAMPLES - ns);
        }
        CHECK(m->beeper.overflow == 0, "overflow %u", m->beeper.overflow);

        /* Rising zero crossings, interpolated, after 0.1 s to settle. */
        const double T = 2048.0 / 75.0;
        double first = -1, last = -1;
        unsigned crossings = 0;
        for (size_t k = 3662; k < ns; k++) {
            if (samples[k - 1] < 0 && samples[k] >= 0) {
                double frac = (double)-samples[k - 1] / (double)(samples[k] - samples[k - 1]);
                double at = ((double)(k - 1) + frac) * T;
                if (first < 0) first = at;
                last = at;
                crossings++;
            }
        }
        double hz = (crossings - 1) * 1e6 / (last - first);
        double want = 1e6 / (2.0 * HALF);
        printf("tone: %.3f Hz measured, %.3f Hz from the cycle count\n", hz, want);
        CHECK(fabs(hz - want) / want < 1e-4, "measured %.3f Hz, expected %.3f Hz", hz, want);

        /* The DC blocker centres it: a 50 % square wave swings about 0. */
        long sum = 0;
        int16_t lo = 0, hi = 0;
        for (size_t k = 3662; k < ns; k++) {
            sum += samples[k];
            if (samples[k] < lo) lo = samples[k];
            if (samples[k] > hi) hi = samples[k];
        }
        double mean = (double)sum / (double)(ns - 3662);
        CHECK(fabs(mean) < 200, "mean %.1f should be near 0", mean);
        CHECK(hi > 7000 && lo < -7000, "swing %d..%d", lo, hi);
    }

    /* ---- silence, and a speaker left high, both rest at 0 ------------ */
    {
        atom_t *m = machine(idle, sizeof(idle));
        size_t ns = 0;
        for (unsigned f = 0; f < 30; f++) {
            atom_run_field(m);
            ns += atom_audio_drain(m, samples + ns, MAX_SAMPLES - ns);
        }
        int nonzero = 0;
        for (size_t k = 0; k < ns; k++) nonzero += samples[k] != 0;
        CHECK(nonzero == 0, "an idle speaker should be silent, %d nonzero", nonzero);

        /* BSR: set PC2 (#B003 <- 0x05) and leave it. A step, then decay. */
        bus_write(m, 0xB003, 0x05);
        CHECK(atom_speaker(m), "BSR should set the speaker bit");
        ns = 0;
        for (unsigned f = 0; f < 60; f++) {
            atom_run_field(m);
            ns += atom_audio_drain(m, samples + ns, MAX_SAMPLES - ns);
        }
        CHECK(samples[1] > 10000, "the step should be heard, got %d", samples[1]);
        CHECK(abs(samples[ns - 1]) <= 1, "a held level should decay to 0, got %d",
              samples[ns - 1]);
    }

    /* ---- nobody draining costs samples, and says so ------------------- */
    {
        atom_t *m = machine(idle, sizeof(idle));
        for (unsigned f = 0; f < 10; f++) atom_run_field(m);
        CHECK(m->beeper.count == ATOM_AUDIO_BUF_LEN, "buffer should be full");
        CHECK(m->beeper.overflow > 0, "the loss should be counted");

        /* A partial drain keeps the order. */
        int16_t a[10];
        m->beeper.buf[10] = 1234;
        CHECK(atom_audio_drain(m, a, 10) == 10, "partial drain");
        CHECK(m->beeper.buf[0] == 1234, "the rest should move up");
        CHECK(m->beeper.count == ATOM_AUDIO_BUF_LEN - 10u, "count after drain");
    }

    /* ---- the rate follows the clock it is given ----------------------- */
    {
        atom_t *m = machine(idle, sizeof(idle));
        atom_audio_set_rate(m, 125000000u, 4096u);
        CHECK(m->beeper.num == 4096u && m->beeper.den == 125u,
              "125 MHz / 4096 should be 4096/125, got %u/%u",
              m->beeper.num, m->beeper.den);
    }

    TEST_DONE();
}
