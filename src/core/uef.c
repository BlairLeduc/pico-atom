/* uef.c — a UEF image as half-cycles (uef.h). */

#include "uef.h"

#include <string.h>

static const char magic[10] = "UEF File!";   /* and its NUL */

static uint32_t le(const uint8_t *p, unsigned n) {
    uint32_t v = 0;
    for (unsigned i = 0; i < n; i++) v |= (uint32_t)p[i] << (8u * i);
    return v;
}

/* UEF stores reals as IEEE 754 singles, little-endian. */
static float le_float(const uint8_t *p) {
    uint32_t v = le(p, 4);
    float f;
    memcpy(&f, &v, sizeof f);
    return f;
}

bool uef_open(uef_t *u, const uint8_t *img, size_t len) {
    memset(u, 0, sizeof *u);
    if (len < UEF_HEADER_LEN || memcmp(img, magic, sizeof magic) != 0) return false;
    u->img = img;
    u->len = (uint32_t)len;
    uef_rewind(u);
    return true;
}

void uef_rewind(uef_t *u) {
    const uint8_t *img = u->img;
    uint32_t len = u->len;
    memset(u, 0, sizeof *u);
    u->img = img;
    u->len = len;
    u->next = UEF_HEADER_LEN;
    u->base_hz = UEF_BASE_HZ;
    u->baud = UEF_BAUD_ATOM;
}

unsigned uef_percent(const uef_t *u) {
    if (u->len <= UEF_HEADER_LEN) return 100;
    uint32_t at = u->id ? u->body + (u->at < u->body_len ? u->at : u->body_len) : u->next;
    if (at > u->len) at = u->len;
    return (unsigned)((uint64_t)(at - UEF_HEADER_LEN) * 100u / (u->len - UEF_HEADER_LEN));
}

static bool open_chunk(uef_t *u) {
    if (u->next + 6u > u->len) return false;
    const uint8_t *h = u->img + u->next;
    u->id = (uint16_t)le(h, 2);
    u->body = u->next + 6u;
    u->body_len = le(h + 2, 4);
    /* A chunk that says it runs past the file is played as far as the
     * file goes. */
    if (u->body_len > u->len - u->body) u->body_len = u->len - u->body;
    u->next = u->body + u->body_len;
    u->at = 0;
    u->part = 0;
    return true;
}

/* A frame of `n` bits, LSB first, to be sent at the current baud. */
static void frame(uef_t *u, uint16_t bits, uint8_t n) {
    u->frame = bits;
    u->frame_left = n;
}

static uint16_t frame_8n1(uint8_t b) {
    return (uint16_t)(((unsigned)b << 1) | 0x200u);   /* start 0, stop 1 */
}

/* A bit at the current baud: base/baud cycles of the base frequency for
 * a 0, twice as many at twice the frequency for a 1 — 4 and 8 at the
 * Atom's 300 (#FC9C). */
static void bit_wave(uef_t *u, unsigned bit) {
    uint32_t cycles = (u->base_hz + u->baud / 2u) / u->baud;
    if (cycles == 0) cycles = 1;
    u->wave = bit ? UEF_WAVE_HIGH : UEF_WAVE_LOW;
    u->wave_left = (bit ? 4u : 2u) * cycles;
}

static void tone(uef_t *u, uint32_t cycles) {
    u->wave = UEF_WAVE_HIGH;
    u->wave_left = 2u * cycles;
}

static void gap(uef_t *u, uint32_t units) {
    u->wave = UEF_WAVE_GAP;
    u->wave_left = units;
}

/* Take the next frame or wave out of the chunk being played. False when
 * the chunk has nothing more to give. */
static bool chunk_step(uef_t *u) {
    const uint8_t *b = u->img + u->body;
    uint32_t n = u->body_len;

    switch (u->id) {
    case 0x0100:                       /* data, 8N1 */
        if (u->at >= n) return false;
        frame(u, frame_8n1(b[u->at++]), 10);
        return true;

    case 0x0104: {                     /* data, the frame given */
        if (n < 3) return false;
        if (u->at == 0) {
            u->fmt_bits = b[0] > 8 ? 8 : b[0];
            u->fmt_parity = b[1];
            /* A negative count is that many stop bits and then an extra
             * short wave: one cycle at twice the base frequency. */
            int8_t stop = (int8_t)b[2];
            u->fmt_stop = (uint8_t)(stop < 0 ? -stop : stop);
            u->fmt_short = stop < 0;
            if (u->fmt_stop > 4) u->fmt_stop = 4;
            u->at = 3;
        }
        if (u->at >= n) return false;
        uint8_t v = b[u->at++];
        uint16_t f = 0;                             /* start bit 0 */
        uint8_t k = 1, ones = 0;
        for (unsigned i = 0; i < u->fmt_bits; i++, k++) {
            unsigned bit = (v >> i) & 1u;
            ones = (uint8_t)(ones + bit);
            f |= (uint16_t)(bit << k);
        }
        if (u->fmt_parity == 'E' || u->fmt_parity == 'O') {
            unsigned p = (ones & 1u) ^ (u->fmt_parity == 'O' ? 1u : 0u);
            f |= (uint16_t)(p << k++);
        }
        for (unsigned i = 0; i < u->fmt_stop; i++) f |= (uint16_t)(1u << k++);
        frame(u, f, k);
        u->frame_tail = u->fmt_short ? 2u : 0u;
        return true;
    }

    case 0x0102: {                     /* explicit bits, LSB first */
        if (n < 1) return false;
        uint32_t total = n * 8u > b[0] ? n * 8u - b[0] : 0;
        uint32_t i = 8u + u->at;                   /* the count byte is counted */
        if (i >= total) return false;
        u->at++;
        frame(u, (uint16_t)((b[i / 8u] >> (i % 8u)) & 1u), 1);
        return true;
    }

    case 0x0110:                       /* carrier, cycles at twice base */
        if (u->at || n < 2) return false;
        u->at = 1;
        tone(u, le(b, 2));
        return true;

    case 0x0111:                       /* carrier, &AA, carrier */
        if (n < 4 || u->part > 2) return false;
        switch (u->part++) {
        case 0: tone(u, le(b, 2)); break;
        case 1: frame(u, frame_8n1(0xAA), 10); break;
        default: tone(u, le(b + 2, 2)); break;
        }
        return true;

    case 0x0112:                       /* gap, in 1/(2 base) s */
        if (u->at || n < 2) return false;
        u->at = 1;
        gap(u, 2u * le(b, 2));
        return true;

    case 0x0116: {                     /* gap, in seconds */
        if (u->at || n < 4) return false;
        u->at = 1;
        float secs = le_float(b);
        if (!(secs > 0.0f) || secs > 3600.0f) return false;
        gap(u, (uint32_t)(secs * 4.0f * (float)u->base_hz + 0.5f));
        return true;
    }

    case 0x0113: {                     /* base frequency */
        if (u->at || n < 4) return false;
        u->at = 1;
        float hz = le_float(b);
        if (hz >= 100.0f && hz <= 10000.0f) u->base_hz = (uint32_t)(hz + 0.5f);
        return false;
    }

    case 0x0114: {                     /* security cycles, MSB first */
        if (n < 5) return false;
        uint32_t count = le(b, 3);
        if (count > (n - 5u) * 8u) count = (n - 5u) * 8u;
        if (u->at >= count) return false;
        uint32_t c = u->at++;
        unsigned bit = (b[5u + c / 8u] >> (7u - c % 8u)) & 1u;
        u->wave = bit ? UEF_WAVE_HIGH : UEF_WAVE_LOW;
        u->wave_left = 2;
        /* 'P': the first or last cycle is a single pulse, half a cycle. */
        if ((c == 0 && b[3] == 'P') || (c + 1u == count && b[4] == 'P')) u->wave_left = 1;
        return true;
    }

    case 0x0117:                       /* baud */
        if (u->at || n < 2) return false;
        u->at = 1;
        if (le(b, 2) >= 50u) u->baud = (uint16_t)le(b, 2);
        return false;

    default:
        return false;
    }
}

bool uef_next(uef_t *u, uint32_t *units, bool *edge) {
    if (!u->img) return false;
    for (;;) {
        if (u->wave_left) {
            if (u->wave == UEF_WAVE_GAP) {
                *units = u->wave_left;
                *edge = false;
                u->wave_left = 0;
            } else {
                *units = u->wave == UEF_WAVE_HIGH ? 1u : 2u;
                *edge = true;
                u->wave_left--;
            }
            return true;
        }
        if (u->frame_left) {
            bit_wave(u, u->frame & 1u);
            u->frame >>= 1;
            u->frame_left--;
            continue;
        }
        if (u->frame_tail) {
            u->wave = UEF_WAVE_HIGH;
            u->wave_left = u->frame_tail;
            u->frame_tail = 0;
            continue;
        }
        if (u->id && chunk_step(u)) continue;
        u->id = 0;
        if (!open_chunk(u)) return false;
    }
}

bool uef_first_name(const uint8_t *img, size_t len, char name[14]) {
    uef_t u;
    if (!uef_open(&u, img, len)) return false;
    unsigned stars = 0, n = 0;
    bool in_name = false;
    while (open_chunk(&u)) {
        uint32_t skip = u.id == 0x0100 ? 0u : u.id == 0x0104 ? 3u : u.body_len;
        for (uint32_t i = skip; i < u.body_len; i++) {
            uint8_t b = img[u.body + i];
            if (!in_name) {
                stars = b == '*' ? stars + 1u : 0u;
                if (stars == 4) { in_name = true; n = 0; }
            } else if (b == '*' && n == 0) {
                /* More framing: the byte before "****" can be a '*'. */
            } else if (b == 0x0D) {
                name[n] = 0;
                return true;
            } else if (n == 13) {
                in_name = false;       /* too long: not a header after all */
                stars = 0;
            } else {
                name[n++] = (char)b;
            }
        }
    }
    return false;
}
