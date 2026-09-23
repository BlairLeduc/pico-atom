/* i8271.h — the Intel 8271 floppy disc controller that AtomDOS drives
 * (design.md §11.4).
 *
 * What the DOS ROM uses, read off it: the command and status register
 * at #0A00, parameter and result at #0A01, reset at #0A02 and data at
 * #0A04 (bus.c, §7.3). Every command goes out with the drive select in
 * bits 7-6 (#E7D2). The DOS puts the part in non-DMA mode (mode
 * register #C1, #E874), so each data byte raises INT with the non-DMA
 * data request, and the Atom has INT on NMI: the DOS points #0200 at
 * its handler, #E87B, which moves one byte through #0A04 per NMI and
 * takes the result when a completion arrives instead. Commands used:
 * SPECIFY, WRITE SPECIAL REGISTER, SEEK, READ DRIVE STATUS, READ DATA
 * and WRITE DATA, variable length. Format, verify and read ID are
 * modelled for the utilities that call them.
 *
 * The chip holds no disc. It knows each drive's geometry, so it decides
 * for itself what is there — sector not found, write protect, not ready
 * — and asks the port only for bytes: a read posts a request for up to a
 * track of sectors and waits, busy, until the port fills buf; a write
 * collects them into buf and then posts its request. The port serves it
 * at a field boundary (main.c), as it serves a tape call.
 *
 * READY follows the head. The chip loads it for a command, which runs
 * the motor, and unloads it after SPECIFY's count of idle revolutions,
 * and READY drops with it — which is how the DOS knows to read the
 * catalogue again (#E731): it trusts its copy while the drive stays
 * ready. Changing the disc unloads a head that was counting down, since
 * the guest is paused while it happens and the count would have run out.
 *
 * Time is the guest's: the caller passes the cycle count in and runs
 * i8271_event when it reaches `due`. Late data is not modelled — a byte
 * the CPU has not taken by the next one is overwritten, and the DOS's
 * handler is fast enough that it never is.
 */
#ifndef PICO_ATOM_I8271_H
#define PICO_ATOM_I8271_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* Register offsets as the bus decodes them. */
#define I8271_REG_STATUS  0u   /* read: status;  write: command   */
#define I8271_REG_RESULT  1u   /* read: result;  write: parameter */
#define I8271_REG_RESET   2u   /* write: bit 0 holds the chip in reset */
#define I8271_REG_DATA    4u   /* #0A04: the data register, on DACK */

/* Status register. */
#define I8271_ST_BUSY     0x80u
#define I8271_ST_CMD_FULL 0x40u
#define I8271_ST_PAR_FULL 0x20u
#define I8271_ST_RES_FULL 0x10u
#define I8271_ST_INT      0x08u
#define I8271_ST_NDR      0x04u   /* non-DMA data request */

/* Completion codes, the ones this model produces. */
#define I8271_OK              0x00u
#define I8271_ERR_DATA_CRC    0x0Eu
#define I8271_ERR_NOT_READY   0x10u
#define I8271_ERR_PROTECT     0x12u
#define I8271_ERR_WRITE_FAULT 0x16u
#define I8271_ERR_NOT_FOUND   0x18u

/* Special registers. */
#define I8271_SR_TRACK0   0x12u   /* surface 0's current track */
#define I8271_SR_MODE     0x17u
#define I8271_SR_TRACK1   0x1Au   /* surface 1's current track */
#define I8271_SR_OUTPUT   0x23u   /* drive control output      */
#define I8271_SR_COUNT    0x40u

#define I8271_OUT_SIDE    0x20u   /* side select, drive control output */
#define I8271_OUT_LOAD    0x08u   /* load head, which runs the motor   */

/* One revolution at 300 rpm, in guest cycles. */
#define I8271_REV_CYCLES  (10u * ATOM_FDC_SECTOR_CYCLES)

#define I8271_NEVER       UINT64_MAX

typedef struct {
    bool    loaded;
    bool    protect;
    uint8_t tracks;     /* per side */
    uint8_t sides;      /* 1 or 2   */
    uint8_t head;       /* the physical track under the head */
} i8271_drive_t;

typedef enum {
    I8271_REQ_NONE = 0,
    I8271_REQ_READ,     /* fill buf with count sectors            */
    I8271_REQ_WRITE,    /* store count sectors from buf           */
} i8271_req_op_t;

typedef struct {
    i8271_req_op_t op;
    uint8_t drive, side, track, sector, count;   /* track is physical */
} i8271_req_t;

typedef struct {
    uint8_t status, result, data;
    uint8_t cmd;             /* bits 5-0 of the command byte    */
    int8_t  drive;           /* selected by bits 7-6, or -1     */
    uint8_t params[5];
    uint8_t nparams, need;
    uint8_t special[I8271_SR_COUNT];
    uint8_t unload_revs;     /* SPECIFY: idle revolutions to head unload */
    bool    unload_armed;    /* idle, counting down to the unload        */
    bool    in_reset;

    /* The command in progress. */
    uint8_t phase;
    uint8_t track;           /* as the command asked for it     */
    uint8_t sector;          /* next sector to move             */
    uint8_t left;            /* sectors still to move           */
    uint8_t code;            /* completion code when it ends    */
    uint16_t pos, end;       /* bytes through buf               */
    bool    out_wanted;      /* write: a byte has been asked for */

    uint64_t due;            /* next event, or I8271_NEVER      */

    i8271_drive_t drv[ATOM_FDC_DRIVES];
    i8271_req_t   req;       /* op NONE unless waiting on the port */
    uint8_t       buf[ATOM_FDC_BUF_LEN];

    uint32_t sectors_read, sectors_written;   /* for the heartbeat */
} i8271_t;

/* Power on: no discs, heads at track 0, nothing in progress. */
void i8271_init(i8271_t *f);

/* The RESET pin: the command in progress is dropped. Discs, heads and
 * the special registers stay. */
void i8271_reset(i8271_t *f);

uint8_t i8271_read(i8271_t *f, uint8_t reg, uint64_t now);
void    i8271_write(i8271_t *f, uint8_t reg, uint8_t v, uint64_t now);

/* Run the event that fell due at or before `now`. */
void i8271_event(i8271_t *f, uint64_t now);

/* The INT pin, which the Atom wires to NMI. */
static inline bool i8271_int(const i8271_t *f) { return (f->status & I8271_ST_INT) != 0; }

/* The bytes the chip is waiting on, or NULL. */
static inline const i8271_req_t *i8271_request(const i8271_t *f) {
    return f->req.op != I8271_REQ_NONE ? &f->req : NULL;
}

/* The port has filled buf for a READ, or stored it for a WRITE; ok is
 * false if the card failed it. */
void i8271_served(i8271_t *f, bool ok, uint64_t now);

/* A disc in a drive, or none. A command on that drive that has left
 * the seek ends: a disc changed under a read is a read that fails. One
 * still seeking finds whatever disc is there when the head settles. */
void i8271_insert(i8271_t *f, unsigned drive, uint8_t tracks, uint8_t sides, bool protect);
void i8271_eject(i8271_t *f, unsigned drive);

/* Busy on a command: a snapshot now would lose it. */
static inline bool i8271_busy(const i8271_t *f) { return (f->status & I8271_ST_BUSY) != 0; }

#endif /* PICO_ATOM_I8271_H */
