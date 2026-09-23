/* tape.c — tape phase 1: the OSLOAD and OSSAVE traps (design.md §11.2).
 *
 * The addresses in the comments are the stock kernel's. What the trap
 * leaves behind in page zero is checked against the ROM routines
 * themselves by test_tape, which runs them over a byte stream.
 */

#include "tape.h"

#include <string.h>

#include "atom.h"
#include "bus.h"

/* The first eight bytes of each handler: PHP, SEI, JSR #F84F (copy the
 * parameter block), PHP, then the two diverge. A MOS whose bytes differ
 * is not the one whose page-zero contract this file reproduces, so the
 * trap stands aside for it. */
static const uint8_t sig_load[8] = { 0x08, 0x78, 0x20, 0x4F, 0xF8, 0x08, 0x20, 0x3E };
static const uint8_t sig_save[8] = { 0x08, 0x78, 0x20, 0x4F, 0xF8, 0x08, 0xA9, 0x06 };

/* Page zero the handlers use. */
#define ZP_BLOCK     0xC9u   /* #C9-#D2: the parameter block (#F84F)   */
#define ZP_HDR       0xD4u   /* #D4-#DB: the last block's header        */
#define ZP_SUM       0xDCu   /* running checksum (#FC23)                */
#define ZP_MODE      0xDDu   /* bit 7 *LOAD, bit 6 nameless (#F94D)     */
#define ZP_NAMEBUF   0xEDu   /* the name read off tape (#FBC9)          */

#define PORT_C       0xB002u

/* Block framing on tape (#FB3B): "****", the name, CR, eight header
 * bytes, the data, a checksum. Every block of a file is 256 bytes but
 * the last. */
#define TAPE_BLOCK   256u

/* ---- ATM ------------------------------------------------------------ */

void atm_header_decode(const uint8_t raw[ATM_HEADER_LEN], atm_header_t *h) {
    memcpy(h->name, raw, ATOM_ATM_NAME_LEN);
    h->name[ATOM_ATM_NAME_LEN] = 0;
    /* Some archives terminate the name with CR rather than NUL padding,
     * the way the MOS stores it; either ends it. */
    for (unsigned i = 0; i < ATOM_ATM_NAME_LEN; i++) {
        if (h->name[i] == 0x0D) { h->name[i] = 0; break; }
    }
    h->load = (uint16_t)(raw[16] | (raw[17] << 8));
    h->exec = (uint16_t)(raw[18] | (raw[19] << 8));
    h->len  = (uint16_t)(raw[20] | (raw[21] << 8));
}

void atm_header_encode(const atm_header_t *h, uint8_t raw[ATM_HEADER_LEN]) {
    memset(raw, 0, ATOM_ATM_NAME_LEN);
    size_t n = strnlen(h->name, ATOM_ATM_NAME_LEN);
    memcpy(raw, h->name, n);
    raw[16] = (uint8_t)h->load; raw[17] = (uint8_t)(h->load >> 8);
    raw[18] = (uint8_t)h->exec; raw[19] = (uint8_t)(h->exec >> 8);
    raw[20] = (uint8_t)h->len;  raw[21] = (uint8_t)(h->len >> 8);
}

bool atm_name_matches(const atm_header_t *h, const char *want) {
    return strncmp(h->name, want, ATOM_ATM_NAME_LEN) == 0;
}

/* ---- guest access --------------------------------------------------- */

/* A read with no side effects: memory as the page table maps it, and
 * the open bus for I/O and holes, which is what a save of I/O space
 * would get on the real machine short of the device's own reply. */
static uint8_t peek(const atom_t *m, uint16_t a) {
    const uint8_t *p = m->page[a >> 8].read;
    return p ? p[a & 0xFFu] : m->open_bus;
}

static void zp_write(atom_t *m, uint8_t zp, uint8_t v) {
    bus_write(m, zp, v);
}

static void zp_write16(atom_t *m, uint8_t zp, uint16_t v) {
    zp_write(m, zp, (uint8_t)v);
    zp_write(m, (uint8_t)(zp + 1u), (uint8_t)(v >> 8));
}

/* The handler was reached by JMP (vector), so the caller's JSR return
 * address is on top of the stack. */
static void rts(atom_t *m) {
    m6502_t *c = &m->cpu;
    c->s++;
    uint16_t lo = peek(m, (uint16_t)(0x0100u | c->s));
    c->s++;
    uint16_t hi = peek(m, (uint16_t)(0x0100u | c->s));
    c->pc = (uint16_t)(((hi << 8) | lo) + 1u);
}

static bool signature(const atom_t *m, uint16_t pc, const uint8_t sig[8]) {
    if (!(m->page_flags[pc >> 8] & PAGE_ROM)) return false;
    for (unsigned i = 0; i < 8; i++) {
        if (peek(m, (uint16_t)(pc + i)) != sig[i]) return false;
    }
    return true;
}

/* ---- the trap -------------------------------------------------------- */

bool tape_trap(atom_t *m) {
    tape_t *t = &m->tape;

    /* BREAK wins: the reset is serviced at this boundary and the request
     * goes with the program that made it. */
    if (m->cpu.reset_pending) {
        t->op = TAPE_NONE;
        t->pass = false;
        return false;
    }
    if (t->op != TAPE_NONE) return true;
    if (t->pass) { t->pass = false; return false; }
    if (!m->cfg.tape_traps) return false;

    uint16_t pc = m->cpu.pc;
    tape_op_t op;
    if (pc == TAPE_OSLOAD_PC && signature(m, pc, sig_load))      op = TAPE_LOAD;
    else if (pc == TAPE_OSSAVE_PC && signature(m, pc, sig_save)) op = TAPE_SAVE;
    else return false;

    /* The block is X,0..9 in page zero, wrapping as LDA &00,X does. */
    uint8_t x = m->cpu.x;
    for (unsigned i = 0; i < sizeof t->block; i++) {
        t->block[i] = peek(m, (uint8_t)(x + i));
    }

    /* The name: at most 13 bytes and a CR, or the MOS's NAME error. */
    uint16_t np = (uint16_t)(t->block[0] | (t->block[1] << 8));
    unsigned n = 0;
    for (;;) {
        uint8_t ch = peek(m, (uint16_t)(np + n));
        if (ch == 0x0D) break;
        if (n == TAPE_NAME_MAX) return false;
        t->name[n++] = (char)ch;
    }
    t->name[n] = 0;

    t->op = op;
    t->own_addr = !(t->block[4] & 0x80u);
    t->addr = (uint16_t)(t->block[2] | (t->block[3] << 8));
    t->done = 0;
    return true;
}

const tape_t *atom_tape_pending(const atom_t *m) {
    return m->tape.op != TAPE_NONE ? &m->tape : NULL;
}

void atom_tape_decline(atom_t *m) {
    m->tape.op = TAPE_NONE;
    m->tape.pass = true;
}

/* Both handlers start by copying the block to #C9 (#F84F). */
static void copy_block(atom_t *m) {
    for (unsigned i = 0; i < sizeof m->tape.block; i++) {
        zp_write(m, (uint8_t)(ZP_BLOCK + i), m->tape.block[i]);
    }
}

/* The sum a block's trailing checksum byte carries: every byte after the
 * sync tone, framing included (#FC23 adds each one as it goes). */
static uint8_t block_sum(const char *name, const uint8_t hdr[8], uint8_t data_sum) {
    unsigned s = 4u * '*' + 0x0Du + data_sum;
    for (const char *p = name; *p; p++) s += (uint8_t)*p;
    for (unsigned i = 0; i < 8; i++) s += hdr[i];
    return (uint8_t)s;
}

/* The ROM's record loop (#FB0D), which decides how a file is cut into
 * blocks and what each block's flags byte says. The flags byte is built
 * by rotating into #D2, which starts as the high byte of the end
 * address: bit 7 "another block follows", bit 6 "this block has data",
 * bit 5 "not the first block", and bits 4-0 whatever was rotated out of
 * the previous value. Those low bits mean nothing, but they are recorded, they are
 * summed into the checksum, and a load leaves them in #DB, so they are
 * reproduced rather than zeroed. Returns the last block's state. */
typedef struct {
    unsigned blocks;
    uint8_t  flags;      /* the last block's, as recorded      */
    uint8_t  len1;       /* its length - 1 (#CF)                */
    uint8_t  d2;         /* #D2 when the loop exits             */
} record_t;

static record_t record(uint16_t start, uint16_t end) {
    uint8_t d3 = (uint8_t)start, d4 = (uint8_t)(start >> 8);
    uint16_t last = (uint16_t)(end - 1u);
    uint8_t d5 = (uint8_t)last, d6 = (uint8_t)(last >> 8);
    uint8_t d2 = (uint8_t)(end >> 8);
    record_t r = { 0, 0, 0, 0 };
    bool more;
    do {
        /* The loop re-enters at #FB0E, past the CLC, with C still set
         * from the ROL that decided to go round again. */
        d2 = (uint8_t)((d2 >> 1) | (r.blocks ? 0x80u : 0u));
        unsigned lo = (unsigned)d5 - d3;
        uint8_t cf = (uint8_t)lo;
        unsigned hi = (unsigned)d6 - d4 - (lo > 0xFFu ? 1u : 0u);
        bool c = hi <= 0xFFu;                               /* no borrow */
        bool z = (uint8_t)hi == 0;
        d2 = (uint8_t)((d2 >> 1) | (c ? 0x80u : 0u));
        bool follow = c && !z;
        if (follow) cf = 0xFF;
        d2 = (uint8_t)((d2 >> 1) | (follow ? 0x80u : 0u));

        r.blocks++;
        r.flags = d2;
        r.len1 = cf;
        d4++;
        more = (d2 & 0x80u) != 0;
        d2 = (uint8_t)((d2 << 1) | 1u);   /* ROL, C set by the block write */
    } while (more);
    r.d2 = d2;
    return r;
}

/* ---- load ------------------------------------------------------------ */

uint16_t atom_tape_load_begin(atom_t *m, const atm_header_t *h) {
    tape_t *t = &m->tape;
    t->hdr = *h;
    t->base = t->own_addr ? h->load : t->addr;
    t->done = 0;
    return t->base;
}

void atom_tape_load_data(atom_t *m, const uint8_t *src, size_t n) {
    tape_t *t = &m->tape;
    if (t->op != TAPE_LOAD) return;
    for (size_t i = 0; i < n && t->done < t->hdr.len; i++, t->done++) {
        bus_write(m, (uint16_t)(t->base + t->done), src[i]);
    }
}

/* What the named path (#F97A) leaves when the last block has been read,
 * so that *RUN's JMP (#D6), BASIC's return to #CD9B, and anything else
 * that looks, find what they would after a real load. The file is taken
 * to have been recorded from where it loads, which is what an ATM
 * header can say; the end address seeds the flags (record). */
void atom_tape_load_end(atom_t *m) {
    tape_t *t = &m->tape;
    if (t->op != TAPE_LOAD) return;
    const atm_header_t *h = &t->hdr;
    record_t r = record(h->load, (uint16_t)(h->load + h->len));
    uint16_t last_off = (uint16_t)((r.blocks - 1u) * TAPE_BLOCK);
    bool data = (r.flags & 0x40u) != 0;
    uint8_t dd = peek(m, ZP_MODE);

    /* The last block's header, in the order #FBE2 reads it into #DB
     * down to #D4: flags, block number, length - 1, exec, load. */
    uint16_t blk_load = (uint16_t)(h->load + last_off);
    uint8_t hdr[8] = {
        r.flags, 0x00, (uint8_t)(r.blocks - 1u), r.len1,
        (uint8_t)(h->exec >> 8), (uint8_t)h->exec,
        (uint8_t)(blk_load >> 8), (uint8_t)blk_load,
    };
    for (unsigned i = 0; i < 8; i++) zp_write(m, (uint8_t)(ZP_HDR + 7u - i), hdr[i]);

    uint8_t data_sum = 0;
    for (uint32_t i = last_off; i < h->len; i++) {
        data_sum = (uint8_t)(data_sum + peek(m, (uint16_t)(t->base + i)));
    }
    uint8_t sum = block_sum(h->name, hdr, data_sum);

    copy_block(m);
    zp_write16(m, ZP_BLOCK + 2u, (uint16_t)(t->base + last_off));   /* #CB */
    zp_write(m, ZP_BLOCK + 5u, sum);                                /* #CE (#F9F4) */
    zp_write16(m, ZP_BLOCK + 7u, (uint16_t)(r.blocks - 1u));        /* #D0 */
    /* #DB is rotated left once the checksum has matched (#FA05). */
    zp_write(m, ZP_HDR + 7u, (uint8_t)((r.flags << 1) | 1u));
    /* The checksum byte is itself added as it is read (#F9F6). */
    zp_write(m, ZP_SUM, (uint8_t)(sum * 2u));
    zp_write(m, ZP_MODE, (uint8_t)(dd >> 2));

    size_t nl = strlen(h->name);
    for (size_t i = 0; i < nl; i++) zp_write(m, (uint8_t)(ZP_NAMEBUF + i), (uint8_t)h->name[i]);
    zp_write(m, (uint8_t)(ZP_NAMEBUF + nl), 0x0D);

    /* "PLAY TAPE" leaves the port at its idle value (#FC40). */
    bus_write(m, PORT_C, 0x07u);

    /* A is the checksum; X is what the block-number check left, which it
     * skips under *LOAD (#F9C9); Y indexed the last data byte. The flags
     * are the caller's, restored by the PLP at #F953. */
    m->cpu.a = sum;
    m->cpu.x = (dd & 0x80u) ? 2u : 0u;
    m->cpu.y = data ? r.len1 : 0u;

    t->op = TAPE_NONE;
    t->served++;
    rts(m);
}

/* ---- save ------------------------------------------------------------ */

void atom_tape_save_header(const atom_t *m, atm_header_t *h) {
    const tape_t *t = &m->tape;
    memset(h, 0, sizeof(*h));
    memcpy(h->name, t->name, strlen(t->name));
    h->load = (uint16_t)(t->block[2] | (t->block[3] << 8));
    h->exec = (uint16_t)(t->block[4] | (t->block[5] << 8));
    uint16_t start = (uint16_t)(t->block[6] | (t->block[7] << 8));
    uint16_t end   = (uint16_t)(t->block[8] | (t->block[9] << 8));
    h->len = (uint16_t)(end - start);
}

size_t atom_tape_save_data(const atom_t *m, uint32_t offset, uint8_t *dst, size_t n) {
    const tape_t *t = &m->tape;
    atm_header_t h;
    atom_tape_save_header(m, &h);
    uint16_t start = (uint16_t)(t->block[6] | (t->block[7] << 8));
    size_t i = 0;
    for (; i < n && offset + i < h.len; i++) {
        dst[i] = peek(m, (uint16_t)(start + offset + i));
    }
    return i;
}

/* What the record loop (#FAF8) leaves after the last block. */
void atom_tape_save_end(atom_t *m) {
    tape_t *t = &m->tape;
    if (t->op != TAPE_SAVE) return;
    uint16_t reload = (uint16_t)(t->block[2] | (t->block[3] << 8));
    uint16_t start  = (uint16_t)(t->block[6] | (t->block[7] << 8));
    uint16_t end    = (uint16_t)(t->block[8] | (t->block[9] << 8));
    record_t r = record(start, end);
    bool data = (r.flags & 0x40u) != 0;
    uint16_t last_off = (uint16_t)((r.blocks - 1u) * TAPE_BLOCK);
    uint16_t blk_load = (uint16_t)(reload + last_off);

    /* The last block as it went to tape: header, then data. */
    uint8_t hdr[8] = {
        r.flags, 0x00, (uint8_t)(r.blocks - 1u), r.len1,
        t->block[5], t->block[4],
        (uint8_t)(blk_load >> 8), (uint8_t)blk_load,
    };
    uint8_t data_sum = 0;
    if (data) {
        for (unsigned i = 0; i <= r.len1; i++) {
            data_sum = (uint8_t)(data_sum + peek(m, (uint16_t)(start + last_off + i)));
        }
    }
    uint8_t sum = block_sum(t->name, hdr, data_sum);

    copy_block(m);
    uint16_t last = (uint16_t)(end - 1u);
    zp_write(m, ZP_BLOCK + 3u, (uint8_t)((reload >> 8) + r.blocks));     /* #CC */
    zp_write(m, ZP_BLOCK + 6u, r.len1);                                  /* #CF */
    zp_write16(m, ZP_BLOCK + 7u, (uint16_t)r.blocks);                    /* #D0 */
    zp_write(m, ZP_BLOCK + 9u, r.d2);                                    /* #D2 */
    zp_write(m, 0xD3u, (uint8_t)start);
    zp_write(m, 0xD4u, (uint8_t)((start >> 8) + r.blocks));
    zp_write16(m, 0xD5u, last);
    zp_write(m, ZP_SUM, (uint8_t)(sum * 2u));

    /* The recorder is switched off after the last block (#FB78). */
    bus_write(m, PORT_C, 0x04u);

    /* A is the checksum just written; X ran the delay loop down; Y
     * indexed the last data byte. The flags are the caller's (#FB39). */
    m->cpu.a = sum;
    m->cpu.x = 0;
    m->cpu.y = data ? r.len1 : 0u;

    t->op = TAPE_NONE;
    t->served++;
    rts(m);
}
