/* snapshot.c — whole-machine state to and from a byte stream
 * (design.md §11.5; the format is in snapshot.h). */

#include "snapshot.h"

#include <string.h>

#include "atom.h"
#include "sha1.h"

static const uint8_t magic[8] = { 'P', 'A', 'T', 'M', 'S', 'N', 'A', 'P' };

/* The payload is produced a page at a time, so nothing larger than a
 * page is ever held: the machine is the buffer. */
#define PIECE ATOM_PAGE_SIZE

/* ---- CRC-32 ---------------------------------------------------------- */

uint32_t snapshot_crc32(uint32_t crc, const uint8_t *p, size_t n) {
    crc = ~crc;
    while (n--) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}

/* ---- little-endian fields ---------------------------------------------- */

static void put16(uint8_t *p, uint16_t v) { p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8); }
static void put32(uint8_t *p, uint32_t v) { put16(p, (uint16_t)v); put16(p + 2, (uint16_t)(v >> 16)); }
static void put64(uint8_t *p, uint64_t v) { put32(p, (uint32_t)v); put32(p + 4, (uint32_t)(v >> 32)); }
static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }
static uint32_t get32(const uint8_t *p) { return get16(p) | ((uint32_t)get16(p + 2) << 16); }
static uint64_t get64(const uint8_t *p) { return get32(p) | ((uint64_t)get32(p + 4) << 32); }

/* ---- the state section ------------------------------------------------ */

enum {
    S_PC = 0, S_A = 2, S_X, S_Y, S_S, S_P,
    S_CYCLES = 7,
    S_IRQ = 15, S_NMI_PENDING, S_NMI_LINE,
    S_PPI = 18,                   /* out_a, out_b, out_c, control, in_c */
    S_VIA = 23,                   /* orb ora ddrb ddra in_a in_b sr acr pcr ifr ier */
    S_T1_LATCH = 34, S_T2_LATCH_LO = 36, S_T1 = 37, S_T2 = 41,
    S_T1_ARMED = 45, S_T2_ARMED,
    S_FLYBACK = 47, S_OPEN_BUS,
    S_BUDGET = 49,
    S_CFG = 53, S_FIELD_HZ = 54,
    S_ROMS = 56,                  /* SHA-1 of the ROM pages, 20 bytes  */
    S_FDC = 76,                   /* mode, output, track 0, track 1, head 0, head 1, unload revs */
    S_VIA2 = 83,                  /* ira irb lines sr_halves, sr_timer (M10) */
    S_END = 89,                   /* the rest is reserved, written zero */
};

_Static_assert(S_END <= SNAP_STATE_LEN, "the state section has outgrown its length");

/* The configuration bits that change what the address space is. */
static uint8_t cfg_bits(const atom_config_t *c) {
    return (uint8_t)((c->block_zero ? 0x01u : 0) | (c->text_space ? 0x02u : 0) |
                     (c->video ? 0x04u : 0) | (c->video_aperture ? 0x08u : 0) |
                     (c->via_fitted ? 0x10u : 0) | (c->atomdos ? 0x20u : 0) |
                     (c->upper_ram ? 0x40u : 0));
}

/* Every ROM page, with its page number, so the same images in different
 * sockets hash differently. */
static void rom_hash(const atom_t *m, uint8_t digest[SHA1_DIGEST_LEN]) {
    sha1_t s;
    sha1_init(&s);
    for (unsigned p = 0; p < ATOM_PAGE_COUNT; p++) {
        if (!(m->page_flags[p] & PAGE_ROM)) continue;
        uint8_t n = (uint8_t)p;
        sha1_update(&s, &n, 1);
        sha1_update(&s, &m->ram[p * ATOM_PAGE_SIZE], ATOM_PAGE_SIZE);
    }
    sha1_final(&s, digest);
}

static void state_encode(const atom_t *m, uint8_t st[SNAP_STATE_LEN]) {
    memset(st, 0, SNAP_STATE_LEN);
    const m6502_t *c = &m->cpu;
    put16(st + S_PC, c->pc);
    st[S_A] = c->a; st[S_X] = c->x; st[S_Y] = c->y; st[S_S] = c->s; st[S_P] = c->p;
    put64(st + S_CYCLES, c->cycles);
    st[S_IRQ] = c->irq_lines;
    st[S_NMI_PENDING] = c->nmi_pending;
    st[S_NMI_LINE] = c->nmi_line;

    const i8255_t *ppi = &m->ppi;
    uint8_t *q = st + S_PPI;
    *q++ = ppi->out_a; *q++ = ppi->out_b; *q++ = ppi->out_c; *q++ = ppi->control;
    *q++ = ppi->in_c;

    const via6522_t *v = &m->via;
    q = st + S_VIA;
    *q++ = v->orb; *q++ = v->ora; *q++ = v->ddrb; *q++ = v->ddra;
    *q++ = v->in_a; *q++ = v->in_b; *q++ = v->sr; *q++ = v->acr; *q++ = v->pcr;
    *q++ = v->ifr; *q++ = v->ier;
    put16(st + S_T1_LATCH, v->t1_latch);
    st[S_T2_LATCH_LO] = v->t2_latch_lo;
    put32(st + S_T1, (uint32_t)v->t1);
    put32(st + S_T2, (uint32_t)v->t2);
    st[S_T1_ARMED] = v->t1_armed;
    st[S_T2_ARMED] = v->t2_armed;
    /* The rest of the part, from M10 (§7.4), written so that zero is
     * the state after a reset: a file from before M10 loads as a VIA
     * with idle lines and a stopped shift register. */
    q = st + S_VIA2;
    *q++ = v->ira; *q++ = v->irb;
    *q++ = (uint8_t)((v->pb7 ? 0 : 0x01u) | (v->ca1 ? 0 : 0x02u) | (v->ca2 ? 0 : 0x04u) |
                     (v->cb1 ? 0 : 0x08u) | (v->cb2 ? 0 : 0x10u));
    *q++ = v->sr_halves;
    put16(q, (uint16_t)v->sr_timer);

    st[S_FLYBACK] = m->in_flyback;
    st[S_OPEN_BUS] = m->open_bus;
    put32(st + S_BUDGET, (uint32_t)m->budget);
    st[S_CFG] = cfg_bits(&m->cfg);
    put16(st + S_FIELD_HZ, (uint16_t)m->cfg.field_hz);
    rom_hash(m, st + S_ROMS);

    /* The 8271's setup, which the DOS writes once at *DOS and a
     * restored machine still counts on: non-DMA mode above all, or the
     * next read never raises an NMI. A command in progress is not
     * saved; snapshot_save refuses while there is one. */
    const i8271_t *f = &m->fdc;
    q = st + S_FDC;
    *q++ = f->special[I8271_SR_MODE];   *q++ = f->special[I8271_SR_OUTPUT];
    *q++ = f->special[I8271_SR_TRACK0]; *q++ = f->special[I8271_SR_TRACK1];
    *q++ = f->drv[0].head;              *q++ = f->drv[1].head;
    *q++ = f->unload_revs;
}

/* ---- the stream -------------------------------------------------------- */

/* One page of the address space as it goes out: RAM as it stands, ROM
 * as zeros, and holes and I/O as zeros too, since they hold nothing. */
static const uint8_t *page_out(const atom_t *m, unsigned p, uint8_t *scratch) {
    if (m->page[p].write && !(m->page_flags[p] & (PAGE_ROM | PAGE_IO))) {
        return &m->ram[p * ATOM_PAGE_SIZE];
    }
    /* Page #0A under AtomDOS is RAM but for the FDC's eight bytes. */
    if (m->page_flags[p] & PAGE_IO && p < 0xB0u) {
        memcpy(scratch, &m->ram[p * ATOM_PAGE_SIZE], PIECE);
        return scratch;
    }
    memset(scratch, 0, PIECE);
    return scratch;
}

snap_status_t snapshot_save(const atom_t *m, snap_write_fn write, void *ctx) {
    if (m->tape.op != TAPE_NONE) return SNAP_BUSY;
    /* The FDC mid-command, or with a completion the DOS has not taken:
     * neither is in the state section. */
    if (i8271_busy(&m->fdc) || i8271_int(&m->fdc)) return SNAP_BUSY;

    uint8_t st[SNAP_STATE_LEN];
    uint8_t scratch[PIECE];
    state_encode(m, st);

    /* The CRC goes in the header, ahead of what it covers, so the
     * payload is produced twice: once to sum, once to send. It is a
     * function of the machine, which nothing changes in between. */
    uint32_t crc = snapshot_crc32(0, st, sizeof st);
    for (unsigned p = 0; p < ATOM_PAGE_COUNT; p++) {
        crc = snapshot_crc32(crc, page_out(m, p, scratch), PIECE);
    }

    uint8_t hdr[SNAP_HEADER_LEN];
    memcpy(hdr, magic, sizeof magic);
    put16(hdr + 8, SNAP_VERSION);
    put16(hdr + 10, SNAP_HEADER_LEN);
    put32(hdr + 12, SNAP_PAYLOAD_LEN);
    put32(hdr + 16, crc);

    if (!write(ctx, hdr, sizeof hdr)) return SNAP_IO;
    if (!write(ctx, st, sizeof st)) return SNAP_IO;
    for (unsigned p = 0; p < ATOM_PAGE_COUNT; p++) {
        if (!write(ctx, page_out(m, p, scratch), PIECE)) return SNAP_IO;
    }
    return SNAP_OK;
}

static snap_status_t read_header(snap_read_fn read, void *ctx, uint32_t *crc) {
    uint8_t hdr[SNAP_HEADER_LEN];
    if (!read(ctx, hdr, sizeof hdr)) return SNAP_IO;
    if (memcmp(hdr, magic, sizeof magic) != 0) return SNAP_NOT_SNAPSHOT;
    if (get16(hdr + 8) > SNAP_VERSION) return SNAP_NEWER;
    if (get16(hdr + 8) != SNAP_VERSION || get16(hdr + 10) != SNAP_HEADER_LEN ||
        get32(hdr + 12) != SNAP_PAYLOAD_LEN) {
        return SNAP_NOT_SNAPSHOT;
    }
    *crc = get32(hdr + 16);
    return SNAP_OK;
}

/* Would this state resume on this machine? */
static snap_status_t compatible(const atom_t *m, const uint8_t st[SNAP_STATE_LEN]) {
    if (st[S_CFG] != cfg_bits(&m->cfg)) return SNAP_OTHER_MACHINE;
    if (get16(st + S_FIELD_HZ) != m->cfg.field_hz) return SNAP_OTHER_MACHINE;
    uint8_t roms[SHA1_DIGEST_LEN];
    rom_hash(m, roms);
    if (memcmp(roms, st + S_ROMS, sizeof roms) != 0) return SNAP_OTHER_ROMS;
    return SNAP_OK;
}

snap_status_t snapshot_check(const atom_t *m, snap_read_fn read, void *ctx) {
    uint32_t want;
    snap_status_t r = read_header(read, ctx, &want);
    if (r != SNAP_OK) return r;

    uint8_t st[SNAP_STATE_LEN];
    uint8_t piece[PIECE];
    if (!read(ctx, st, sizeof st)) return SNAP_IO;
    uint32_t crc = snapshot_crc32(0, st, sizeof st);
    for (unsigned p = 0; p < ATOM_PAGE_COUNT; p++) {
        if (!read(ctx, piece, PIECE)) return SNAP_IO;
        crc = snapshot_crc32(crc, piece, PIECE);
    }
    if (crc != want) return SNAP_CORRUPT;
    return compatible(m, st);
}

snap_status_t snapshot_load(atom_t *m, snap_read_fn read, void *ctx) {
    if (m->tape.op != TAPE_NONE) return SNAP_BUSY;
    uint32_t want;
    snap_status_t r = read_header(read, ctx, &want);
    if (r != SNAP_OK) return r;
    uint8_t st[SNAP_STATE_LEN];
    if (!read(ctx, st, sizeof st)) return SNAP_IO;
    r = compatible(m, st);
    if (r != SNAP_OK) return r;

    /* From here the machine changes. snapshot_check has read these same
     * bytes and found them whole; a read failing now is the card going
     * away between the passes, and leaves a machine that needs a reset. */
    uint8_t piece[PIECE];
    for (unsigned p = 0; p < ATOM_PAGE_COUNT; p++) {
        if (!read(ctx, piece, PIECE)) return SNAP_IO;
        if (m->page_flags[p] & PAGE_ROM) continue;
        if (!m->page[p].write && !(m->page_flags[p] & PAGE_IO)) continue;
        if ((m->page_flags[p] & PAGE_IO) && p >= 0xB0u) continue;
        memcpy(&m->ram[p * ATOM_PAGE_SIZE], piece, PIECE);
    }

    m6502_t *c = &m->cpu;
    uint64_t was = c->cycles;
    c->pc = get16(st + S_PC);
    c->a = st[S_A]; c->x = st[S_X]; c->y = st[S_Y]; c->s = st[S_S]; c->p = st[S_P];
    c->cycles = get64(st + S_CYCLES);
    /* The tape is not machine state: it stays where it is in the deck,
     * and carries on from the restored clock (§11.3). */
    cassette_retime(&m->cas, was, c->cycles);
    c->irq_lines = st[S_IRQ];
    c->nmi_pending = st[S_NMI_PENDING] != 0;
    c->nmi_line = st[S_NMI_LINE] != 0;
    c->reset_pending = false;

    i8255_t *ppi = &m->ppi;
    const uint8_t *q = st + S_PPI;
    ppi->out_a = *q++; ppi->out_b = *q++; ppi->out_c = *q++; ppi->control = *q++;
    ppi->in_c = *q++;

    via6522_t *v = &m->via;
    q = st + S_VIA;
    v->orb = *q++; v->ora = *q++; v->ddrb = *q++; v->ddra = *q++;
    v->in_a = *q++; v->in_b = *q++; v->sr = *q++; v->acr = *q++; v->pcr = *q++;
    v->ifr = *q++; v->ier = *q++;
    v->t1_latch = get16(st + S_T1_LATCH);
    v->t2_latch_lo = st[S_T2_LATCH_LO];
    v->t1 = (int32_t)get32(st + S_T1);
    v->t2 = (int32_t)get32(st + S_T2);
    v->t1_armed = st[S_T1_ARMED] != 0;
    v->t2_armed = st[S_T2_ARMED] != 0;
    q = st + S_VIA2;
    v->ira = *q++; v->irb = *q++;
    uint8_t lines = *q++;
    v->pb7 = !(lines & 0x01u); v->ca1 = !(lines & 0x02u); v->ca2 = !(lines & 0x04u);
    v->cb1 = !(lines & 0x08u); v->cb2 = !(lines & 0x10u);
    uint8_t halves = *q++;
    v->sr_halves = halves > 16u ? 16u : halves;
    v->sr_timer = (int16_t)get16(q);
    v->ca2_pulses = v->cb2_pulses = 0;

    m->in_flyback = st[S_FLYBACK] != 0;
    m->open_bus = st[S_OPEN_BUS];
    m->budget = (int32_t)get32(st + S_BUDGET);

    /* Keys are whatever the keyboard says now, not what it said then. */
    memset(m->key_col, 0, sizeof m->key_col);
    m->key_shift = m->key_ctrl = m->key_rept = false;
    atom_refresh_ppi_inputs(m);
    /* Port C bit 5 is the deck's, not the file's. Bit 4 stays as it was
     * read: the next read recomputes it from the clock. */
    m->ppi.in_c = (uint8_t)((m->ppi.in_c & ~I8255_IN_C_CASSETTE_DATA) |
                            (m->cas.level ? I8255_IN_C_CASSETTE_DATA : 0u));

    m->tape.op = TAPE_NONE;
    m->tape.pass = false;
    m->tape.cue_play = false;

    /* The discs stay in their drives, as the tape stays in the deck. */
    i8271_t *f = &m->fdc;
    i8271_reset(f);
    q = st + S_FDC;
    f->special[I8271_SR_MODE] = *q++;   f->special[I8271_SR_OUTPUT] = *q++;
    f->special[I8271_SR_TRACK0] = *q++; f->special[I8271_SR_TRACK1] = *q++;
    f->drv[0].head = *q++;              f->drv[1].head = *q++;
    /* SPECIFY's head unload, without which a disc change goes unseen
     * (#E731). A snapshot from before it was saved reads zero: no unload. */
    f->unload_revs = *q++;
    /* The head comes back unloaded, the drive not ready, so the DOS
     * reads the catalogue again: the disc may not be the one it was. */
    f->special[I8271_SR_OUTPUT] &= (uint8_t)~I8271_OUT_LOAD;

    /* The loudspeaker starts again from the restored clock and level, at
     * the rate the port set: cycles per sample num / den, as it was. */
    beeper_t *b = &m->beeper;
    bool dc_block = b->dc_block;
    beeper_init(b, c->cycles, atom_speaker(m), b->num, b->den, 1u);
    b->dc_block = dc_block;
    return SNAP_OK;
}

const char *snapshot_status_str(snap_status_t st) {
    switch (st) {
    case SNAP_OK:            return "OK";
    case SNAP_IO:            return "READ OR WRITE FAILED";
    case SNAP_NOT_SNAPSHOT:  return "NOT A SNAPSHOT";
    case SNAP_NEWER:         return "FROM A NEWER VERSION";
    case SNAP_CORRUPT:       return "DAMAGED (CRC)";
    case SNAP_OTHER_MACHINE: return "OTHER MACHINE CONFIG";
    case SNAP_OTHER_ROMS:    return "OTHER ROMS";
    case SNAP_BUSY:          return "TAPE OR DISC BUSY";
    }
    return "?";
}
