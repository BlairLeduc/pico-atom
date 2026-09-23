/* sha1.c — SHA-1 per FIPS 180-4 (scope in sha1.h). */

#include "sha1.h"

#include <string.h>

static uint32_t rol(uint32_t x, unsigned n) {
    return (x << n) | (x >> (32u - n));
}

static void compress(sha1_t *s, const uint8_t *p) {
    uint32_t w[80];
    for (unsigned i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 |
               (uint32_t)p[4 * i + 2] << 8 | (uint32_t)p[4 * i + 3];
    }
    for (unsigned i = 16; i < 80; i++) {
        w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }

    uint32_t a = s->h[0], b = s->h[1], c = s->h[2], d = s->h[3], e = s->h[4];
    for (unsigned i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999u; }
        else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1u; }
        else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDCu; }
        else             { f = b ^ c ^ d;                   k = 0xCA62C1D6u; }
        uint32_t t = rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = rol(b, 30); b = a; a = t;
    }
    s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e;
}

void sha1_init(sha1_t *s) {
    s->h[0] = 0x67452301u;
    s->h[1] = 0xEFCDAB89u;
    s->h[2] = 0x98BADCFEu;
    s->h[3] = 0x10325476u;
    s->h[4] = 0xC3D2E1F0u;
    s->length = 0;
    s->used = 0;
}

void sha1_update(sha1_t *s, const void *data, size_t len) {
    const uint8_t *p = data;
    s->length += len;
    while (len > 0) {
        uint32_t take = 64u - s->used;
        if (take > len) take = (uint32_t)len;
        memcpy(&s->block[s->used], p, take);
        s->used += take;
        p += take;
        len -= take;
        if (s->used == 64u) {
            compress(s, s->block);
            s->used = 0;
        }
    }
}

void sha1_final(sha1_t *s, uint8_t digest[SHA1_DIGEST_LEN]) {
    uint64_t bits = s->length * 8u;
    uint8_t pad = 0x80u;
    sha1_update(s, &pad, 1);
    pad = 0;
    while (s->used != 56u) sha1_update(s, &pad, 1);
    uint8_t len_be[8];
    for (unsigned i = 0; i < 8; i++) len_be[i] = (uint8_t)(bits >> (56u - 8u * i));
    sha1_update(s, len_be, 8);

    for (unsigned i = 0; i < 5; i++) {
        digest[4 * i]     = (uint8_t)(s->h[i] >> 24);
        digest[4 * i + 1] = (uint8_t)(s->h[i] >> 16);
        digest[4 * i + 2] = (uint8_t)(s->h[i] >> 8);
        digest[4 * i + 3] = (uint8_t)s->h[i];
    }
}

void sha1(const void *data, size_t len, uint8_t digest[SHA1_DIGEST_LEN]) {
    sha1_t s;
    sha1_init(&s);
    sha1_update(&s, data, len);
    sha1_final(&s, digest);
}
