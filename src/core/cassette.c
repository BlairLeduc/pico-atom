/* cassette.c — the cassette input at signal level (cassette.h). */

#include "cassette.h"

#include <string.h>

#include "config.h"
#include "hot.h"

void cassette_init(cassette_t *c) {
    memset(c, 0, sizeof *c);
}

bool cassette_insert(cassette_t *c, const uint8_t *img, size_t len) {
    uint32_t ref = c->hz_ref;
    cassette_init(c);
    c->hz_ref = ref;           /* the reference runs whatever the deck holds */
    if (!uef_open(&c->uef, img, len)) return false;
    c->loaded = true;
    return true;
}

void cassette_eject(cassette_t *c) {
    uint32_t ref = c->hz_ref;
    bool level = c->level;
    cassette_init(c);
    c->hz_ref = ref;
    c->level = level;
}

/* Fetch the next piece of waveform and work out when it ends. Units are
 * a quarter of the base period (uef.h); the remainder carries so that a
 * 2400 Hz half-cycle is 208 or 209 cycles and never drifts. */
static void schedule(cassette_t *c) {
    uint32_t units;
    bool edge;
    if (!uef_next(&c->uef, &units, &edge)) {
        c->playing = false;
        c->ended = true;
        return;
    }
    uint32_t den = 4u * c->uef.base_hz;
    if (c->acc >= den) c->acc = 0;          /* the base frequency changed */
    uint64_t num = (uint64_t)units * ATOM_CPU_HZ + c->acc;
    c->edge += num / den;
    c->acc = (uint32_t)(num % den);
    c->edge_toggles = edge;
}

static void advance(cassette_t *c, uint64_t now) {
    while (c->playing && now >= c->edge) {
        if (c->edge_toggles) {
            c->level = !c->level;
            c->edges++;
        }
        schedule(c);
    }
}

void cassette_play(cassette_t *c, uint64_t now, bool on) {
    if (!c->loaded) return;
    if (on == c->playing) return;
    if (on) {
        if (c->ended) return;               /* rewind first, as on a deck */
        c->edge = now + c->paused_left;
        c->playing = true;
        advance(c, now);
    } else {
        advance(c, now);
        c->paused_left = c->edge > now ? c->edge - now : 0;
        c->playing = false;
    }
}

void cassette_rewind(cassette_t *c, uint64_t now) {
    if (!c->loaded) return;
    bool was = c->playing;
    uef_rewind(&c->uef);
    c->playing = false;
    c->ended = false;
    c->edge_toggles = false;
    c->paused_left = 0;
    c->acc = 0;
    if (was) cassette_play(c, now, true);
}

void cassette_retime(cassette_t *c, uint64_t from, uint64_t to) {
    advance(c, from);
    if (c->playing) c->edge = to + (c->edge - from);
    /* The reference's base is always a multiple of its period, so its
     * phase is the cycle count's alone; a restored machine reads bit 4
     * as the original did. */
    c->hz_ref = (uint32_t)(to - to % ATOM_CASSETTE_REF_CYCLES);
    c->ref_next = (uint32_t)to;     /* the next read recomputes bit 4 */
}

bool ATOM_HOT1(cassette_input)(cassette_t *c, uint64_t now) {
    advance(c, now);
    return c->level;
}

/* Kept in 32 bits against a base that follows the clock, so the divide
 * is the hardware's and not a 64-bit library call: the MOS reads port C
 * in its tightest loops (§12.1). The base only ever moves by whole
 * periods from zero, so the phase is the cycle count's modulo the
 * period, across 32-bit wraps too, as long as reads come less than
 * 2^32 cycles apart. */
bool ATOM_HOT1(cassette_ref_2400)(cassette_t *c, uint64_t now) {
    uint32_t d = (uint32_t)now - c->hz_ref;
    if (d >= ATOM_CASSETTE_REF_CYCLES) {
        uint32_t r = d % ATOM_CASSETTE_REF_CYCLES;
        c->hz_ref += d - r;
        d = r;
    }
    bool high = d < ATOM_CASSETTE_REF_CYCLES / 2u;
    c->ref_next = c->hz_ref + (high ? ATOM_CASSETTE_REF_CYCLES / 2u : ATOM_CASSETTE_REF_CYCLES);
    return high;
}

unsigned cassette_percent(const cassette_t *c) {
    if (!c->loaded) return 0;
    return c->ended ? 100u : uef_percent(&c->uef);
}
