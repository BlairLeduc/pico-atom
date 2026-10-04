/* cassette.c — the cassette input at signal level (cassette.h). */

#include "cassette.h"

#include <string.h>

#include "config.h"
#include "hot.h"

void cassette_init(cassette_t *c) {
    memset(c, 0, sizeof *c);
    cassette_set_clock(c, 1u);
}

void cassette_set_clock(cassette_t *c, unsigned mhz) {
    c->cpu_hz = ATOM_CPU_HZ * mhz;
    c->ref = ATOM_CASSETTE_REF_CYCLES * mhz;
}

/* Empty the deck, keeping what belongs to the machine: its clock, and
 * the reference, which runs whatever the deck holds. */
static void empty(cassette_t *c) {
    uint32_t hz_ref = c->hz_ref, cpu_hz = c->cpu_hz, ref = c->ref;
    memset(c, 0, sizeof *c);
    c->hz_ref = hz_ref;
    c->cpu_hz = cpu_hz;
    c->ref = ref;
}

bool cassette_insert(cassette_t *c, const uint8_t *img, size_t len) {
    empty(c);
    if (!uef_open(&c->uef, img, len)) return false;
    c->loaded = true;
    return true;
}

bool cassette_insert_rw(cassette_t *c, uint8_t *img, size_t len, size_t cap) {
    if (!cassette_insert(c, img, len)) return false;
    c->wbuf = img;
    c->cap = (uint32_t)cap;
    return true;
}

void cassette_eject(cassette_t *c) {
    bool level = c->level;
    empty(c);
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
    uint64_t num = (uint64_t)units * c->cpu_hz + c->acc;
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
    /* While recording the recorder has the tape (§11.3). */
    if (!c->loaded || c->rec.on) return;
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
    if (!c->loaded || c->rec.on) return;
    (void)now;
    uef_rewind(&c->uef);
    c->playing = false;
    c->ended = false;
    c->edge_toggles = false;
    c->paused_left = 0;
    c->acc = 0;
}

static void rec_advance(cassette_t *c, uint64_t to);

void cassette_retime(cassette_t *c, uint64_t from, uint64_t to) {
    advance(c, from);
    if (c->rec.on) {
        rec_advance(c, from);
        c->rec.last_edge = to - (from - c->rec.last_edge);
        c->rec.at = to;
    }
    if (c->playing) c->edge = to + (c->edge - from);
    /* The reference's base is always a multiple of its period, so its
     * phase is the cycle count's alone; a restored machine reads bit 4
     * as the original did. */
    c->hz_ref = (uint32_t)(to - to % c->ref);
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
    if (d >= c->ref) {
        uint32_t r = d % c->ref;
        c->hz_ref += d - r;
        d = r;
    }
    bool high = d < c->ref / 2u;
    c->ref_next = c->hz_ref + (high ? c->ref / 2u : c->ref);
    return high;
}

unsigned cassette_percent(const cassette_t *c) {
    if (!c->loaded) return 0;
    return c->ended ? 100u : uef_percent(&c->uef);
}

/* ---- the recorder (§11.3) ------------------------------------------------ */

/* The reference's period and half, in this clock's cycles. */
#define REF  (c->ref)
#define HALF (c->ref / 2u)

/* A half-cycle by its length: 2400 Hz is HALF, 208 cycles at 1 MHz, and
 * 1200 Hz REF, 416. The boundary between them is halfway; anything
 * shorter than half a short one, or longer than a long one and a
 * quarter, is no signal at all. */
enum { H_SHORT, H_LONG, H_GAP };

static unsigned half_kind(const cassette_t *c, uint64_t d) {
    if (d < REF * 3u / 8u || d >= REF * 5u / 4u) return H_GAP;
    return d < REF * 3u / 4u ? H_SHORT : H_LONG;
}

/* The writer pauses between the bytes of a block (#FC88), leaving the
 * reference gated on for a period or two. A run of tone this short
 * between two bytes is that pause, not carrier: it is left out, and the
 * bytes stay in one &0100 chunk. The reader waits for eight long
 * half-cycles before every byte (#FBF4), so it needs no tone there. */
#define REC_PAUSE_HALVES 32u

/* The line at cycle t: bit 0, or the reference while bit 1 gates it.
 * The reference's phase is the cycle count's alone (cassette_retime). */
static bool rec_line(const cassette_t *c, uint8_t out, uint64_t t) {
    if (!(out & 0x01u)) return false;
    if (!(out & 0x02u)) return true;
    return t % REF < HALF;
}

static bool room(cassette_t *c, uint32_t n) {
    if (c->uef.len + n <= c->cap) return true;
    c->rec.full = true;
    return false;
}

static void put_le(uint8_t *p, uint32_t v, unsigned n) {
    for (unsigned i = 0; i < n; i++) p[i] = (uint8_t)(v >> (8u * i));
}

/* A chunk with a 16-bit body: &0110's cycles or &0112's gap. */
static bool put16(cassette_t *c, uint16_t id, uint32_t v) {
    if (!room(c, 8u)) return false;
    uint8_t *p = c->wbuf + c->uef.len;
    put_le(p, id, 2);
    put_le(p + 2, 2, 4);
    put_le(p + 6, v, 2);
    c->uef.len += 8u;
    c->dirty = true;
    return true;
}

static void put_long(cassette_t *c, uint16_t id, uint32_t v) {
    c->rec.data_at = 0;
    while (v) {
        uint32_t n = v > 0xFFFFu ? 0xFFFFu : v;
        if (!put16(c, id, n)) return;
        v -= n;
    }
}

/* Carrier, in cycles of 2400 Hz: two half-cycles each. */
static void put_tone(cassette_t *c) {
    put_long(c, 0x0110, (c->rec.tone + 1u) / 2u);
    c->rec.tone = 0;
}

static void put_gap(cassette_t *c) {
    put_long(c, 0x0112, c->rec.gap);
    c->rec.gap = 0;
}

/* One byte onto the open &0100 chunk, opening one if need be. The
 * chunk's length is kept right after every byte, so a tape that runs
 * out of room mid-block is still a whole image. */
static void put_byte(cassette_t *c, uint8_t v) {
    cassette_rec_t *r = &c->rec;
    if (!r->data_at) {
        if (!room(c, 7u)) return;
        r->data_at = c->uef.len;
        put_le(c->wbuf + c->uef.len, 0x0100, 2);
        put_le(c->wbuf + c->uef.len + 2, 0, 4);
        c->uef.len += 6u;
    } else if (!room(c, 1u)) {
        return;
    }
    c->wbuf[c->uef.len++] = v;
    uint8_t *len = c->wbuf + r->data_at + 2;
    put_le(len, (len[0] | (len[1] << 8) | ((uint32_t)len[2] << 16) | ((uint32_t)len[3] << 24)) + 1u, 4);
    c->dirty = true;
}

/* A byte framed: the tone or silence before it goes first. */
static void rec_byte(cassette_t *c, uint8_t v) {
    cassette_rec_t *r = &c->rec;
    r->bytes++;
    if (r->gap) put_gap(c);
    if (r->tone) {
        if (r->data_at && r->tone < REC_PAUSE_HALVES) r->tone = 0;
        else put_tone(c);
    }
    put_byte(c, v);
}

/* One half-cycle of kind k and length d, decoded as the reader at
 * #FBEE would: a byte is a start bit of eight long half-cycles, eight
 * data bits and a stop bit, each eight long for a 0 or sixteen short
 * for a 1, LSB first (#FC7C). Outside a frame, short ones are carrier
 * and a gap is silence. A half-cycle that breaks a frame drops it,
 * counted, and is read again as though no frame had begun. */
static void rec_half(cassette_t *c, unsigned k, uint64_t d) {
    cassette_rec_t *r = &c->rec;
    if (r->framing) {
        bool fits = r->halves == 0 ? k != H_GAP : k == r->kind;
        if (fits) {
            if (r->halves == 0) r->kind = (uint8_t)k;
            if (++r->halves < (r->kind == H_LONG ? 8u : 16u)) return;
            if (r->kind == H_SHORT) r->frame |= (uint16_t)(1u << r->bit);
            r->halves = 0;
            if (++r->bit < 10u) return;
            r->framing = false;
            if (r->frame & 0x200u) rec_byte(c, (uint8_t)(r->frame >> 1));
            else r->errors++;
            return;
        }
        r->framing = false;
        r->errors++;
    }
    switch (k) {
    case H_SHORT:
        if (r->gap) put_gap(c);
        r->tone++;
        break;
    case H_GAP:
        if (r->tone) put_tone(c);
        r->data_at = 0;
        r->gap += (uint32_t)(d * 2400u / c->cpu_hz);
        break;
    default:
        /* A start bit's first half-cycle. */
        r->framing = true;
        r->bit = 0;
        r->kind = H_LONG;
        r->halves = 1;
        r->frame = 0;
        break;
    }
}

/* `n` more short half-cycles of exactly HALF: the gated reference. Whole
 * bits at a time where a frame is reading them, so that a long run of
 * tone costs a few steps and not one per half-cycle. */
static void rec_shorts(cassette_t *c, uint64_t n) {
    cassette_rec_t *r = &c->rec;
    while (n && r->framing) {
        if (r->halves && r->kind != H_SHORT) {
            rec_half(c, H_SHORT, HALF);
            n--;
            continue;
        }
        uint32_t want = 16u - r->halves - 1u;   /* all but the last, which ends the bit */
        uint32_t k = n - 1u < want ? (uint32_t)(n - 1u) : want;
        r->kind = H_SHORT;
        r->halves = (uint8_t)(r->halves + k);
        n -= k;
        rec_half(c, H_SHORT, HALF);
        n--;
    }
    if (!n) return;
    if (r->gap) put_gap(c);
    r->tone += (uint32_t)n;
}

static void rec_edge(cassette_t *c, uint64_t t) {
    uint64_t d = t - c->rec.last_edge;
    c->rec.last_edge = t;
    rec_half(c, half_kind(c, d), d);
}

/* The line from rec.at up to `to` under rec.out. An edge is where the
 * line differs from the cycle before: at rec.at if the source has just
 * changed, and on every multiple of HALF while the reference is gated
 * on. */
static void rec_advance(cassette_t *c, uint64_t to) {
    cassette_rec_t *r = &c->rec;
    if (to <= r->at) return;
    if (r->full) { r->at = to; return; }
    uint64_t a = r->at;
    bool l = rec_line(c, r->out, a);
    if (l != r->level) rec_edge(c, a);
    if ((r->out & 0x03u) == 0x03u) {
        uint64_t f = (a / HALF + 1u) * HALF;
        if (f < to) {
            rec_edge(c, f);
            uint64_t n = (to - 1u - f) / HALF;
            if (n) {
                rec_shorts(c, n);
                r->last_edge = f + n * HALF;
            }
        }
        l = rec_line(c, r->out, to - 1u);
    }
    r->level = l;
    r->at = to;
}

void cassette_output(cassette_t *c, uint64_t now, uint8_t out_c) {
    if (!c->rec.on) return;
    rec_advance(c, now);
    c->rec.out = out_c & 0x03u;
}

static void rec_stop(cassette_t *c, uint64_t now) {
    cassette_rec_t *r = &c->rec;
    rec_advance(c, now);
    if (!r->full) {
        if (r->framing) r->errors++;
        r->framing = false;
        /* The silence since the last edge, which no edge closes. */
        if ((r->out & 0x03u) != 0x03u && half_kind(c, now - r->last_edge) == H_GAP)
            rec_half(c, H_GAP, now - r->last_edge);
        if (r->tone) put_tone(c);
        if (r->gap) put_gap(c);
    }
    r->on = false;
    r->data_at = 0;
    /* Recording forward leaves the tape at the end of what it wrote. */
    uef_to_end(&c->uef, c->uef.len);
    c->playing = false;
    c->ended = true;
    c->edge_toggles = false;
    c->paused_left = 0;
    c->acc = 0;
}

cas_rec_status_t cassette_record(cassette_t *c, uint64_t now, bool on, uint8_t out_c) {
    if (!on) {
        if (c->rec.on) rec_stop(c, now);
        return CAS_REC_OK;
    }
    if (c->rec.on) return CAS_REC_OK;
    cas_rec_status_t st = !c->loaded ? CAS_REC_NO_TAPE
                        : !c->wbuf ? CAS_REC_PROTECTED
                        : c->uef.len + 16u > c->cap ? CAS_REC_FULL : CAS_REC_OK;
    c->refused = st;
    if (st != CAS_REC_OK) return st;

    cassette_play(c, now, false);
    cassette_rec_t *r = &c->rec;
    memset(r, 0, sizeof *r);
    r->on = true;
    r->out = out_c & 0x03u;
    r->at = now;
    r->last_edge = now;
    r->level = rec_line(c, r->out, now);
    return CAS_REC_OK;
}

unsigned cassette_room_percent(const cassette_t *c) {
    if (!c->cap) return 0;
    return (unsigned)((uint64_t)c->uef.len * 100u / c->cap);
}
