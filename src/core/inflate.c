/* inflate.c — gzip (RFC 1952) around deflate (RFC 1951), into a flat
 * buffer (inflate.h).
 *
 * Canonical Huffman codes are decoded a bit at a time, the way Mark
 * Adler's puff does it: slower than a table-driven decoder and a
 * fraction of the memory, which is the right trade for a tape image
 * decompressed once, from the menu.
 */

#include "inflate.h"

#include <stdbool.h>
#include <string.h>

#include "snapshot.h"   /* snapshot_crc32: the CRC-32 gzip uses */

#define MAXBITS   15u
#define MAXLCODES 286u
#define MAXDCODES 30u
#define FIXLCODES 288u

typedef struct {
    uint16_t count[MAXBITS + 1];   /* codes of each length               */
    uint16_t symbol[FIXLCODES];    /* symbols in canonical order         */
} huffman_t;

static struct {
    inflate_get_fn get;
    void          *ctx;
    uint32_t       bitbuf;
    unsigned       bitcnt;
    uint8_t       *out;
    size_t         cap, n;
    gz_status_t    err;            /* first error; later reads return 0  */

    huffman_t      lencode, distcode;
    uint16_t       lengths[MAXLCODES + MAXDCODES];
} s;

static int byte(void) {
    if (s.err != GZ_OK) return 0;
    int c = s.get(s.ctx);
    if (c < 0) { s.err = GZ_TRUNCATED; return 0; }
    return c & 0xFF;
}

static uint32_t bits(unsigned need) {
    while (s.bitcnt < need) {
        s.bitbuf |= (uint32_t)byte() << s.bitcnt;
        s.bitcnt += 8;
    }
    uint32_t v = s.bitbuf & ((1u << need) - 1u);
    s.bitbuf >>= need;
    s.bitcnt -= need;
    return v;
}

static void put(uint8_t b) {
    if (s.n >= s.cap) {
        if (s.err == GZ_OK) s.err = GZ_TOO_BIG;
        return;
    }
    s.out[s.n++] = b;
}

/* ---- blocks ------------------------------------------------------------ */

static void stored(void) {
    s.bitbuf = 0;          /* the rest of the current byte is padding */
    s.bitcnt = 0;
    unsigned len  = (unsigned)byte();
    len |= (unsigned)byte() << 8;
    unsigned nlen = (unsigned)byte();
    nlen |= (unsigned)byte() << 8;
    if (s.err != GZ_OK) return;
    if (len != (~nlen & 0xFFFFu)) { s.err = GZ_BAD_DATA; return; }
    while (len-- && s.err == GZ_OK) put((uint8_t)byte());
}

/* Build a canonical code from its lengths. False if it is over-
 * subscribed; an incomplete code is allowed, as deflate allows it for a
 * code with a single symbol. */
static bool construct(huffman_t *h, const uint16_t *length, unsigned n) {
    uint16_t offs[MAXBITS + 1];
    memset(h->count, 0, sizeof h->count);
    for (unsigned sym = 0; sym < n; sym++) h->count[length[sym]]++;
    if (h->count[0] == n) return true;   /* no codes: an empty distance code */

    int left = 1;
    for (unsigned len = 1; len <= MAXBITS; len++) {
        left <<= 1;
        left -= h->count[len];
        if (left < 0) return false;
    }
    offs[1] = 0;
    for (unsigned len = 1; len < MAXBITS; len++) offs[len + 1] = (uint16_t)(offs[len] + h->count[len]);
    for (unsigned sym = 0; sym < n; sym++) {
        if (length[sym] != 0) h->symbol[offs[length[sym]]++] = (uint16_t)sym;
    }
    return true;
}

static int decode(const huffman_t *h) {
    int code = 0, first = 0, index = 0;
    for (unsigned len = 1; len <= MAXBITS; len++) {
        code |= (int)bits(1);
        int count = h->count[len];
        if (code - count < first) return h->symbol[index + (code - first)];
        index += count;
        first += count;
        first <<= 1;
        code <<= 1;
        if (s.err != GZ_OK) return -1;
    }
    return -1;             /* ran out of codes */
}

static const uint16_t len_base[29] = {
    3, 4, 5, 6, 7, 8, 9, 10, 11, 13, 15, 17, 19, 23, 27, 31,
    35, 43, 51, 59, 67, 83, 99, 115, 131, 163, 195, 227, 258 };
static const uint8_t len_extra[29] = {
    0, 0, 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 2, 2, 2, 2,
    3, 3, 3, 3, 4, 4, 4, 4, 5, 5, 5, 5, 0 };
static const uint16_t dist_base[30] = {
    1, 2, 3, 4, 5, 7, 9, 13, 17, 25, 33, 49, 65, 97, 129, 193,
    257, 385, 513, 769, 1025, 1537, 2049, 3073, 4097, 6145,
    8193, 12289, 16385, 24577 };
static const uint8_t dist_extra[30] = {
    0, 0, 0, 0, 1, 1, 2, 2, 3, 3, 4, 4, 5, 5, 6, 6,
    7, 7, 8, 8, 9, 9, 10, 10, 11, 11, 12, 12, 13, 13 };

static void codes(void) {
    for (;;) {
        int sym = decode(&s.lencode);
        if (s.err != GZ_OK) return;
        if (sym < 0) { s.err = GZ_BAD_DATA; return; }
        if (sym < 256) { put((uint8_t)sym); continue; }
        if (sym == 256) return;

        sym -= 257;
        if (sym >= 29) { s.err = GZ_BAD_DATA; return; }
        unsigned len = len_base[sym] + bits(len_extra[sym]);
        int dsym = decode(&s.distcode);
        if (s.err != GZ_OK) return;
        if (dsym < 0 || dsym >= 30) { s.err = GZ_BAD_DATA; return; }
        size_t dist = dist_base[dsym] + bits(dist_extra[dsym]);
        if (dist > s.n) { s.err = GZ_BAD_DATA; return; }
        /* Byte by byte: a copy may overlap its own output. */
        while (len-- && s.err == GZ_OK) put(s.out[s.n - dist]);
    }
}

static void fixed(void) {
    unsigned sym = 0;
    for (; sym < 144; sym++) s.lengths[sym] = 8;
    for (; sym < 256; sym++) s.lengths[sym] = 9;
    for (; sym < 280; sym++) s.lengths[sym] = 7;
    for (; sym < FIXLCODES; sym++) s.lengths[sym] = 8;
    construct(&s.lencode, s.lengths, FIXLCODES);
    for (sym = 0; sym < MAXDCODES; sym++) s.lengths[sym] = 5;
    construct(&s.distcode, s.lengths, MAXDCODES);
    codes();
}

static void dynamic(void) {
    static const uint8_t order[19] = {
        16, 17, 18, 0, 8, 7, 9, 6, 10, 5, 11, 4, 12, 3, 13, 2, 14, 1, 15 };
    unsigned nlen  = bits(5) + 257u;
    unsigned ndist = bits(5) + 1u;
    unsigned ncode = bits(4) + 4u;
    if (s.err != GZ_OK) return;
    if (nlen > MAXLCODES || ndist > MAXDCODES) { s.err = GZ_BAD_DATA; return; }

    unsigned i = 0;
    for (; i < ncode; i++) s.lengths[order[i]] = (uint16_t)bits(3);
    for (; i < 19; i++) s.lengths[order[i]] = 0;
    if (!construct(&s.lencode, s.lengths, 19)) { s.err = GZ_BAD_DATA; return; }

    i = 0;
    while (i < nlen + ndist) {
        int sym = decode(&s.lencode);
        if (s.err != GZ_OK) return;
        if (sym < 0) { s.err = GZ_BAD_DATA; return; }
        if (sym < 16) { s.lengths[i++] = (uint16_t)sym; continue; }
        uint16_t len = 0;
        unsigned rep;
        if (sym == 16) {
            if (i == 0) { s.err = GZ_BAD_DATA; return; }
            len = s.lengths[i - 1];
            rep = 3u + bits(2);
        } else if (sym == 17) {
            rep = 3u + bits(3);
        } else {
            rep = 11u + bits(7);
        }
        if (i + rep > nlen + ndist) { s.err = GZ_BAD_DATA; return; }
        while (rep--) s.lengths[i++] = len;
    }
    if (s.lengths[256] == 0) { s.err = GZ_BAD_DATA; return; }   /* no end code */

    if (!construct(&s.lencode, s.lengths, nlen) ||
        !construct(&s.distcode, s.lengths + nlen, ndist)) {
        s.err = GZ_BAD_DATA;
        return;
    }
    codes();
}

/* ---- gzip ----------------------------------------------------------------- */

#define FHCRC    0x02u
#define FEXTRA   0x04u
#define FNAME    0x08u
#define FCOMMENT 0x10u
#define FRESERVED 0xE0u

static uint32_t le32(void) {
    uint32_t v = 0;
    for (unsigned i = 0; i < 4; i++) v |= (uint32_t)byte() << (8u * i);
    return v;
}

gz_status_t gunzip(inflate_get_fn get, void *ctx, uint8_t *out, size_t cap, size_t *out_len) {
    memset(&s, 0, sizeof s);
    s.get = get;
    s.ctx = ctx;
    s.out = out;
    s.cap = cap;
    *out_len = 0;

    uint8_t hdr[10];
    for (unsigned i = 0; i < sizeof hdr; i++) hdr[i] = (uint8_t)byte();
    if (s.err != GZ_OK) return GZ_NOT_GZIP;
    if (hdr[0] != 0x1Fu || hdr[1] != 0x8Bu || hdr[2] != 8u || (hdr[3] & FRESERVED))
        return GZ_NOT_GZIP;
    uint8_t flg = hdr[3];
    if (flg & FEXTRA) {
        unsigned xlen = (unsigned)byte();
        xlen |= (unsigned)byte() << 8;
        while (xlen-- && s.err == GZ_OK) (void)byte();
    }
    if (flg & FNAME)    while (s.err == GZ_OK && byte() != 0) {}
    if (flg & FCOMMENT) while (s.err == GZ_OK && byte() != 0) {}
    if (flg & FHCRC)    { (void)byte(); (void)byte(); }

    bool last;
    do {
        last = bits(1) != 0;
        unsigned type = bits(2);
        if (s.err != GZ_OK) break;
        if (type == 0)      stored();
        else if (type == 1) fixed();
        else if (type == 2) dynamic();
        else                s.err = GZ_BAD_DATA;
    } while (!last && s.err == GZ_OK);

    *out_len = s.n;
    if (s.err != GZ_OK) return s.err;

    /* The trailer starts on the next byte boundary; what is left of the
     * current byte is padding. */
    s.bitbuf = 0;
    s.bitcnt = 0;
    uint32_t crc = le32();
    uint32_t isize = le32();
    if (s.err != GZ_OK) return s.err;
    if (isize != (uint32_t)s.n || crc != snapshot_crc32(0, out, s.n)) return GZ_BAD_CHECK;
    return GZ_OK;
}

const char *gunzip_status_str(gz_status_t st) {
    switch (st) {
    case GZ_OK:        return "ok";
    case GZ_NOT_GZIP:  return "not a gzip file";
    case GZ_TRUNCATED: return "truncated";
    case GZ_BAD_DATA:  return "corrupt";
    case GZ_TOO_BIG:   return "too big for the buffer";
    case GZ_BAD_CHECK: return "CRC or length mismatch";
    }
    return "?";
}
