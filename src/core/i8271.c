/* i8271.c — the floppy disc controller behind AtomDOS (i8271.h,
 * design.md §11.4). */

#include "i8271.h"

#include <string.h>

/* Commands, bits 5-0 of the command byte. The 128-byte forms and the
 * scans are not modelled; the DOS uses the variable-length forms. */
enum {
    CMD_WRITE      = 0x0B,
    CMD_WRITE_DEL  = 0x0F,
    CMD_READ       = 0x13,
    CMD_READ_DEL   = 0x17,
    CMD_READ_ID    = 0x1B,
    CMD_VERIFY     = 0x1F,
    CMD_FORMAT     = 0x23,
    CMD_SEEK       = 0x29,
    CMD_DRIVE_STAT = 0x2C,
    CMD_SPECIFY    = 0x35,
    CMD_WRITE_SR   = 0x3A,
    CMD_READ_SR    = 0x3D,
};

enum {
    P_IDLE = 0,
    P_PARAMS,     /* collecting parameters                  */
    P_SEEK,       /* stepping; due is when the head settles */
    P_WAIT,       /* a request is with the port             */
    P_READ,       /* bytes of buf to the CPU                */
    P_WRITE,      /* bytes from the CPU into buf            */
    P_VERIFY,     /* sectors pass under the head            */
    P_IDS,        /* READ ID: four bytes a sector           */
    P_FORMAT,     /* FORMAT: four ID bytes a sector in      */
    P_DONE,       /* due is when the completion arrives     */
};

/* The gap between one sector's last byte and the next one's first. */
#define GAP_CYCLES (ATOM_FDC_SECTOR_CYCLES - ATOM_DISC_SECTOR_LEN * ATOM_FDC_BYTE_CYCLES)

static uint8_t param_count(uint8_t cmd) {
    switch (cmd) {
    case CMD_WRITE: case CMD_WRITE_DEL: case CMD_READ: case CMD_READ_DEL:
    case CMD_READ_ID: case CMD_VERIFY:  return 3;
    case CMD_FORMAT:                    return 5;
    case CMD_SEEK: case CMD_READ_SR:    return 1;
    case CMD_WRITE_SR:                  return 2;
    case CMD_SPECIFY:                   return 4;
    default:                            return 0;
    }
}

void i8271_init(i8271_t *f) {
    memset(f, 0, sizeof(*f));
    f->drive = -1;
    f->due = I8271_NEVER;
}

void i8271_reset(i8271_t *f) {
    f->status = 0;
    f->result = 0;
    f->phase = P_IDLE;
    f->nparams = f->need = 0;
    f->req.op = I8271_REQ_NONE;
    f->due = I8271_NEVER;
    f->unload_armed = false;
}

/* ---- ending a command -------------------------------------------------- */

/* The result goes in and INT rises: the DOS's NMI handler sees no data
 * request and takes the completion path (#E88B). The head stays loaded
 * for SPECIFY's count of revolutions, then unloads in the idle phase. */
static void complete(i8271_t *f, uint8_t code, uint64_t now) {
    f->phase = P_IDLE;
    f->unload_armed = f->unload_revs != 0;
    f->due = f->unload_armed ? now + (uint64_t)f->unload_revs * I8271_REV_CYCLES : I8271_NEVER;
    f->req.op = I8271_REQ_NONE;
    f->result = code;
    f->status = I8271_ST_RES_FULL | I8271_ST_INT;
}

static void finish_at(i8271_t *f, uint8_t code, uint64_t when) {
    f->phase = P_DONE;
    f->code = code;
    f->due = when;
}

/* The idle count stops; its event goes if it is the one pending. */
static void disarm(i8271_t *f) {
    if (f->unload_armed && f->phase == P_IDLE) f->due = I8271_NEVER;
    f->unload_armed = false;
}

/* A command with a result and no interrupt. */
static void answer(i8271_t *f, uint8_t v) {
    f->phase = P_IDLE;
    f->result = v;
    f->status = I8271_ST_RES_FULL;
}

/* ---- the drive ----------------------------------------------------------- */

static i8271_drive_t *selected(i8271_t *f) {
    return f->drive >= 0 ? &f->drv[f->drive] : NULL;
}

static uint8_t *track_reg(i8271_t *f) {
    return &f->special[f->drive == 1 ? I8271_SR_TRACK1 : I8271_SR_TRACK0];
}

static uint8_t side(const i8271_t *f) {
    return (f->special[I8271_SR_OUTPUT] & I8271_OUT_SIDE) ? 1u : 0u;
}

/* READY is one line on the drive cable, driven by whichever drive is
 * selected, and the DOS tests bit 2 for either drive (#E78B), so both
 * RDY inputs read it. A drive is ready with a disc in and the motor
 * running, which the head load does. */
static uint8_t drive_status(i8271_t *f) {
    const i8271_drive_t *d = selected(f);
    if (!d) return 0;
    uint8_t s = 0;
    if (d->loaded && (f->special[I8271_SR_OUTPUT] & I8271_OUT_LOAD)) s |= 0x44u;   /* RDY1, RDY0 */
    if (d->protect)   s |= 0x08u;
    if (d->head == 0) s |= 0x02u;   /* TRK0 */
    return s;
}

/* Step to the command's track. The 8271 keeps a track register per
 * surface and moves the head by the difference, except that track 0
 * steps out until the TRK0 input says it is there. */
static uint32_t seek(i8271_t *f, uint8_t target) {
    i8271_drive_t *d = selected(f);
    uint8_t *reg = track_reg(f);
    int head = d ? d->head : 0;
    int to = target == 0 ? 0 : head + (int)target - (int)*reg;
    if (to < 0) to = 0;
    if (to > (int)ATOM_DISC_TRACKS_MAX + 2) to = (int)ATOM_DISC_TRACKS_MAX + 2;
    uint32_t steps = (uint32_t)(to > head ? to - head : head - to);
    if (d) d->head = (uint8_t)to;
    *reg = target;
    return steps ? steps * ATOM_FDC_STEP_CYCLES + ATOM_FDC_SETTLE_CYCLES : 0u;
}

/* How many of `want` sectors from `first` exist under the head: the
 * sector IDs on a track carry its physical number, so a head that is
 * not where the command's track says finds none of them. */
static uint8_t sectors_here(const i8271_t *f, uint8_t first, uint8_t want) {
    const i8271_drive_t *d = &f->drv[f->drive];
    if (d->head >= d->tracks || side(f) >= d->sides) return 0;
    if (d->head != f->track || first >= ATOM_DISC_SECTORS) return 0;
    uint8_t n = (uint8_t)(ATOM_DISC_SECTORS - first);
    return want < n ? want : n;
}

/* ---- data commands -------------------------------------------------------- */

static bool is_read(uint8_t c)  { return c == CMD_READ || c == CMD_READ_DEL; }
static bool is_write(uint8_t c) { return c == CMD_WRITE || c == CMD_WRITE_DEL; }

static void post(i8271_t *f, i8271_req_op_t op, uint8_t n) {
    f->req.op = op;
    f->req.drive = (uint8_t)f->drive;
    f->req.side = side(f);
    f->req.track = f->drv[f->drive].head;
    f->req.sector = f->sector;
    f->req.count = n;
    f->phase = P_WAIT;
    f->due = I8271_NEVER;
}

/* The next piece of a read or write: up to a track's worth of what is
 * left, or the end of the command. */
static void next_chunk(i8271_t *f, uint64_t now) {
    if (f->left == 0) {
        finish_at(f, I8271_OK, now + GAP_CYCLES);
        return;
    }
    uint8_t n = sectors_here(f, f->sector, f->left);
    if (n == 0) {
        /* A revolution looking for it, then the error. */
        finish_at(f, I8271_ERR_NOT_FOUND, now + 10u * ATOM_FDC_SECTOR_CYCLES);
        return;
    }
    f->pos = 0;
    f->end = (uint16_t)(n * ATOM_DISC_SECTOR_LEN);
    if (is_read(f->cmd)) {
        post(f, I8271_REQ_READ, n);
    } else {
        f->phase = P_WRITE;
        f->out_wanted = false;
        f->due = now + GAP_CYCLES;
    }
}

/* The head is on the track: check the drive, then start moving data. */
static void on_track(i8271_t *f, uint64_t now) {
    i8271_drive_t *d = selected(f);
    if (!d || !d->loaded) {
        finish_at(f, I8271_ERR_NOT_READY, now + ATOM_FDC_CMD_CYCLES);
        return;
    }
    bool writes = is_write(f->cmd) || f->cmd == CMD_FORMAT;
    if (writes && d->protect) {
        finish_at(f, I8271_ERR_PROTECT, now + ATOM_FDC_CMD_CYCLES);
        return;
    }

    if (f->cmd == CMD_SEEK) {
        finish_at(f, I8271_OK, now + ATOM_FDC_CMD_CYCLES);
        return;
    }

    if (f->cmd == CMD_READ_ID) {
        f->left = f->params[2] & 0x1Fu;
        f->sector = 0;
        f->pos = 0;
        f->phase = P_IDS;
        f->due = now + GAP_CYCLES;
        return;
    }

    if (f->cmd == CMD_FORMAT) {
        /* Four ID bytes a sector from the CPU, then the track is
         * written. The IDs are taken, not honoured: the image is Acorn's
         * format, sectors 0-9 of 256 bytes. */
        f->left = f->params[2] & 0x1Fu;
        f->pos = 0;
        f->end = (uint16_t)(4u * f->left);
        f->out_wanted = false;
        f->phase = P_FORMAT;
        f->due = now + GAP_CYCLES;
        return;
    }

    /* READ, WRITE, VERIFY: bits 7-5 of the third parameter are the
     * sector size, 128 << N. The images hold 256-byte sectors, so any
     * other size reads a CRC that does not match. */
    f->sector = f->params[1];
    f->left = f->params[2] & 0x1Fu;
    if ((f->params[2] >> 5) != 1u && f->left) {
        finish_at(f, I8271_ERR_DATA_CRC, now + ATOM_FDC_SECTOR_CYCLES);
        return;
    }
    if (f->cmd == CMD_VERIFY) {
        uint8_t n = f->left ? sectors_here(f, f->sector, f->left) : 0;
        uint8_t code = n == f->left ? I8271_OK : I8271_ERR_NOT_FOUND;
        uint32_t turns = n == f->left ? n : 10u;
        finish_at(f, code, now + (uint64_t)turns * ATOM_FDC_SECTOR_CYCLES + GAP_CYCLES);
        return;
    }
    next_chunk(f, now);
}

/* ---- commands ------------------------------------------------------------- */

static void execute(i8271_t *f, uint64_t now) {
    switch (f->cmd) {
    case CMD_SPECIFY:
        /* Step rate, settle and head load time: taken, not modelled
         * (config.h). The high nibble of the last parameter is the idle
         * revolutions before the head unloads: #CA from the DOS. No
         * result. */
        if (f->params[0] == 0x0Du) f->unload_revs = f->params[3] >> 4;
        f->phase = P_IDLE;
        f->status = 0;
        return;

    case CMD_WRITE_SR:
        f->special[f->params[0] & (I8271_SR_COUNT - 1u)] = f->params[1];
        /* The DOS loading the head itself, to wait for READY (#E75B),
         * holds it: the idle count stops. */
        if ((f->params[0] & (I8271_SR_COUNT - 1u)) == I8271_SR_OUTPUT) disarm(f);
        f->phase = P_IDLE;
        f->status = 0;
        return;

    case CMD_READ_SR:
        answer(f, f->special[f->params[0] & (I8271_SR_COUNT - 1u)]);
        return;

    case CMD_DRIVE_STAT:
        answer(f, drive_status(f));
        return;

    case CMD_SEEK: case CMD_READ: case CMD_READ_DEL: case CMD_WRITE:
    case CMD_WRITE_DEL: case CMD_VERIFY: case CMD_READ_ID: case CMD_FORMAT:
        f->special[I8271_SR_OUTPUT] |= I8271_OUT_LOAD;
        f->unload_armed = false;
        f->track = f->params[0];
        f->status = I8271_ST_BUSY;
        f->phase = P_SEEK;
        f->due = now + ATOM_FDC_CMD_CYCLES + seek(f, f->track);
        return;

    default:
        /* Not modelled: the scans, the 128-byte forms, and whatever
         * else. They end at once as though the sector were missing. */
        f->status = I8271_ST_BUSY;
        finish_at(f, I8271_ERR_NOT_FOUND, now + ATOM_FDC_CMD_CYCLES);
        return;
    }
}

uint8_t i8271_read(i8271_t *f, uint8_t reg, uint64_t now) {
    (void)now;
    switch (reg) {
    case I8271_REG_STATUS:
        return f->status;
    case I8271_REG_RESULT:
        f->status &= (uint8_t)~(I8271_ST_RES_FULL | I8271_ST_INT);
        return f->result;
    case I8271_REG_DATA:
        f->status &= (uint8_t)~(I8271_ST_NDR | I8271_ST_INT);
        return f->data;
    default:
        return 0xFFu;
    }
}

void i8271_write(i8271_t *f, uint8_t reg, uint8_t v, uint64_t now) {
    switch (reg) {
    case I8271_REG_STATUS:
        if (f->in_reset || (f->status & I8271_ST_BUSY)) return;
        f->cmd = v & 0x3Fu;
        /* Select 0 is bit 6, select 1 bit 7 (#E7D2). */
        f->drive = (v & 0x40u) ? 0 : (v & 0x80u) ? 1 : -1;
        f->nparams = 0;
        f->need = param_count(f->cmd);
        f->status = I8271_ST_BUSY;
        f->phase = P_PARAMS;
        if (f->need == 0) execute(f, now);
        return;
    case I8271_REG_RESULT:
        if (f->phase != P_PARAMS) return;
        f->params[f->nparams++] = v;
        if (f->nparams == f->need) execute(f, now);
        return;
    case I8271_REG_RESET:
        /* Held in reset while bit 0 is set; the DOS writes 1 then 0
         * (#E000). */
        f->in_reset = (v & 1u) != 0;
        if (f->in_reset) i8271_reset(f);
        return;
    case I8271_REG_DATA:
        f->data = v;
        f->status &= (uint8_t)~(I8271_ST_NDR | I8271_ST_INT);
        return;
    default:
        return;
    }
}

/* A byte for the CPU: INT with the data request (non-DMA mode). */
static void offer(i8271_t *f, uint8_t v) {
    f->data = v;
    f->status |= I8271_ST_INT | I8271_ST_NDR;
}

/* Ask the CPU for a byte. */
static void want(i8271_t *f) {
    f->status |= I8271_ST_INT | I8271_ST_NDR;
    f->out_wanted = true;
}

void i8271_event(i8271_t *f, uint64_t now) {
    f->due = I8271_NEVER;
    switch (f->phase) {
    case P_SEEK:
        on_track(f, now);
        return;

    case P_READ:
        offer(f, f->buf[f->pos++]);
        if (f->pos % ATOM_DISC_SECTOR_LEN == 0) {
            f->sector++;
            f->left--;
            f->sectors_read++;
            if (f->pos == f->end) {
                next_chunk(f, now + ATOM_FDC_BYTE_CYCLES);
                return;
            }
            f->due = now + ATOM_FDC_BYTE_CYCLES + GAP_CYCLES;
            return;
        }
        f->due = now + ATOM_FDC_BYTE_CYCLES;
        return;

    case P_WRITE:
        /* The byte asked for last time is the one the data register
         * holds now, taken or not. */
        if (f->out_wanted) {
            f->status &= (uint8_t)~(I8271_ST_NDR | I8271_ST_INT);
            f->buf[f->pos++] = f->data;
            f->out_wanted = false;
            if (f->pos == f->end) {
                post(f, I8271_REQ_WRITE, (uint8_t)(f->end / ATOM_DISC_SECTOR_LEN));
                return;
            }
            if (f->pos % ATOM_DISC_SECTOR_LEN == 0) {
                f->due = now + GAP_CYCLES;
                return;
            }
        }
        want(f);
        f->due = now + ATOM_FDC_BYTE_CYCLES;
        return;

    case P_IDS: {
        /* C H R N: the physical track, the side, the sector, 256 bytes. */
        const i8271_drive_t *d = selected(f);
        uint8_t id[4] = { d->head, side(f), f->sector, 1u };
        offer(f, id[f->pos++]);
        if (f->pos == 4) {
            f->pos = 0;
            f->sector = (uint8_t)((f->sector + 1u) % ATOM_DISC_SECTORS);
            if (--f->left == 0) {
                finish_at(f, I8271_OK, now + GAP_CYCLES);
                return;
            }
            f->due = now + ATOM_FDC_SECTOR_CYCLES;
            return;
        }
        f->due = now + ATOM_FDC_BYTE_CYCLES;
        return;
    }

    case P_FORMAT:
        if (f->out_wanted) {
            f->status &= (uint8_t)~(I8271_ST_NDR | I8271_ST_INT);
            f->out_wanted = false;
            if (++f->pos == f->end) {
                /* The track, written with the filler byte. What the
                 * image holds of it is what a read finds. */
                const i8271_drive_t *d = selected(f);
                uint8_t n = f->left < ATOM_DISC_SECTORS ? f->left : ATOM_DISC_SECTORS;
                if (d->head >= d->tracks || side(f) >= d->sides || n == 0) {
                    finish_at(f, I8271_ERR_WRITE_FAULT, now + ATOM_FDC_SECTOR_CYCLES);
                    return;
                }
                memset(f->buf, 0xE5, (size_t)n * ATOM_DISC_SECTOR_LEN);
                f->sector = 0;
                f->left = n;
                post(f, I8271_REQ_WRITE, n);
                return;
            }
        }
        want(f);
        f->due = now + (f->pos % 4u == 0 && f->pos ? ATOM_FDC_SECTOR_CYCLES : ATOM_FDC_BYTE_CYCLES);
        return;

    case P_DONE:
        complete(f, f->code, now);
        return;

    case P_IDLE:
        /* The idle count ran out: the head unloads, the motor stops. */
        if (f->unload_armed) f->special[I8271_SR_OUTPUT] &= (uint8_t)~I8271_OUT_LOAD;
        f->unload_armed = false;
        return;

    default:
        return;
    }
}

void i8271_served(i8271_t *f, bool ok, uint64_t now) {
    if (f->phase != P_WAIT) return;
    i8271_req_op_t op = f->req.op;
    uint8_t n = f->req.count;
    f->req.op = I8271_REQ_NONE;

    if (op == I8271_REQ_READ) {
        if (!ok) {
            finish_at(f, I8271_ERR_DATA_CRC, now + ATOM_FDC_SECTOR_CYCLES);
            return;
        }
        f->phase = P_READ;
        f->pos = 0;
        f->due = now + GAP_CYCLES;
        return;
    }

    if (!ok) {
        finish_at(f, I8271_ERR_WRITE_FAULT, now + ATOM_FDC_SECTOR_CYCLES);
        return;
    }
    f->sectors_written += n;
    f->sector = (uint8_t)(f->sector + n);
    f->left = (uint8_t)(f->left - n);
    if (f->cmd == CMD_FORMAT) f->left = 0;
    next_chunk(f, now);
}

void i8271_insert(i8271_t *f, unsigned drive, uint8_t tracks, uint8_t sides, bool protect) {
    if (drive >= ATOM_FDC_DRIVES) return;
    i8271_eject(f, drive);
    i8271_drive_t *d = &f->drv[drive];
    d->loaded = true;
    d->tracks = tracks;
    d->sides = sides;
    d->protect = protect;
}

void i8271_eject(i8271_t *f, unsigned drive) {
    if (drive >= ATOM_FDC_DRIVES) return;
    f->drv[drive].loaded = false;
    /* Changing a disc takes longer than the idle count, but the guest is
     * paused while it happens (§13), so an unload that was counting down
     * happens now: READY drops and the DOS reads the catalogue of
     * whatever goes in (#E731). A head the DOS is holding loaded, waiting
     * for READY, stays loaded, and the disc put in satisfies it. */
    if (f->unload_armed) {
        f->special[I8271_SR_OUTPUT] &= (uint8_t)~I8271_OUT_LOAD;
        disarm(f);
    }
    /* A command on it that has left the seek fails as the disc goes. */
    if (f->drive == (int8_t)drive && f->phase > P_SEEK && f->phase != P_DONE) {
        f->req.op = I8271_REQ_NONE;
        f->phase = P_DONE;
        f->code = I8271_ERR_NOT_READY;
        /* Due at once: the caller's next check runs it. */
        f->due = 0;
    }
}
