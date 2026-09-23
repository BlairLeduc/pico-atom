/* beeper.c — the loudspeaker bit to PCM (design.md §9.3). */

#include "beeper.h"

#include <string.h>

/* The DC blocker's pole, Q15: 0.995, a corner near 29 Hz at 36.6 kHz —
 * below anything the PicoCalc's speaker reproduces. */
#define BEEPER_HP_R 32604

static uint64_t gcd64(uint64_t a, uint64_t b) {
    while (b) {
        uint64_t t = a % b;
        a = b;
        b = t;
    }
    return a;
}

void beeper_init(beeper_t *b, uint64_t now, bool level, uint32_t cpu_hz,
                 uint32_t rate_num, uint32_t rate_den) {
    memset(b, 0, sizeof(*b));
    if (rate_num == 0 || rate_den == 0 || cpu_hz == 0) {
        rate_num = ATOM_AUDIO_RATE_NUM;
        rate_den = ATOM_AUDIO_RATE_DEN;
        cpu_hz   = ATOM_CPU_HZ;
    }

    /* Cycles per sample = cpu_hz / (rate_num / rate_den), reduced. At
     * 150 MHz and 4096 clocks a sample that is 2048/75 exactly. */
    uint64_t num = (uint64_t)cpu_hz * rate_den;
    uint64_t den = rate_num;
    uint64_t g = gcd64(num, den);
    num /= g;
    den /= g;
    /* A clock with no common factor could leave the fraction too wide
     * for the 32-bit units below; coarsen it rather than overflow. No
     * clk_sys this build uses gets here. */
    while (num > 0x7FFFFFFFu || den > 0x7FFFFFFFu) {
        num >>= 1;
        den >>= 1;
    }
    if (den == 0) den = 1;
    if (num == 0) num = 1;

    b->num = (uint32_t)num;
    b->den = (uint32_t)den;
    b->q   = (uint32_t)(num / den);
    b->r   = (uint32_t)(num % den);
    b->scale = (((uint64_t)BEEPER_FULL_SCALE << 32) + num / 2u) / num;

    b->start = now;
    b->level = level;
    b->dc_block = true;
}

static void emit(beeper_t *b, uint32_t high) {
    int32_t x = (int32_t)(((uint64_t)high * b->scale + 0x80000000u) >> 32);
    int32_t y = x;
    if (b->dc_block) {
        /* y[n] = x[n] - x[n-1] + R y[n-1], with y carried in Q8 so the
         * truncation does not leave a standing offset. */
        b->hp_y = (x - b->hp_x) * 256 +
                  (int32_t)(((int64_t)b->hp_y * BEEPER_HP_R) >> 15);
        b->hp_x = x;
        y = b->hp_y / 256;
    }
    if (y > INT16_MAX) y = INT16_MAX;
    if (y < INT16_MIN) y = INT16_MIN;

    if (b->count < ATOM_AUDIO_BUF_LEN) b->buf[b->count++] = (int16_t)y;
    else b->overflow++;
}

void beeper_advance(beeper_t *b, uint64_t now) {
    /* num is zero only before beeper_init, which would never leave the
     * loop below. */
    if (b->num == 0 || now <= b->start) return;

    /* Units from the start of the current sample to `now`. The sample
     * began no later than the last boundary passed, so this is never
     * negative; the guard is for a caller that goes backwards. */
    uint64_t t = (now - b->start) * b->den;
    if (t < b->start_frac) return;
    t -= b->start_frac;

    while (t >= b->num) {
        uint32_t high = b->high;
        if (b->level) high += b->num - b->pos;
        emit(b, high);

        t -= b->num;
        b->start += b->q;
        b->start_frac += b->r;
        if (b->start_frac >= b->den) {
            b->start_frac -= b->den;
            b->start++;
        }
        b->pos = 0;
        b->high = 0;
    }

    if (t > b->pos) {
        if (b->level) b->high += (uint32_t)t - b->pos;
        b->pos = (uint32_t)t;
    }
}

void beeper_set_level(beeper_t *b, uint64_t now, bool level) {
    if (level == b->level) return;
    beeper_advance(b, now);
    b->level = level;
    b->edges++;
}

size_t beeper_drain(beeper_t *b, int16_t *dst, size_t max) {
    size_t n = b->count < max ? b->count : max;
    memcpy(dst, b->buf, n * sizeof(b->buf[0]));
    b->count -= (uint32_t)n;
    if (b->count) memmove(b->buf, b->buf + n, b->count * sizeof(b->buf[0]));
    return n;
}
