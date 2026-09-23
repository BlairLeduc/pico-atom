/* test_i8271.c — the 8271 on its own, no ROM (i8271.h, design.md §11.4).
 *
 * test_disc holds the chip to the DOS; this holds the parts the DOS does
 * not reach, and the timing the DOS only needs to be fast enough for:
 * a byte every ATOM_FDC_BYTE_CYCLES, the track register against the
 * head, READ ID, VERIFY, FORMAT, READY against the head load, and
 * reset.
 */

#include <string.h>

#include "i8271.h"
#include "test_util.h"

static i8271_t f;
static uint64_t now;

/* Run events until the command completes, serving reads with a pattern
 * that says which sector it is. Returns the bytes the CPU was offered;
 * `first_gap` gets the cycles between the first two. */
static unsigned run(uint8_t *got, unsigned max, uint64_t *first_gap) {
    unsigned n = 0;
    uint64_t last = 0;
    for (int guard = 0; guard < 100000; guard++) {
        const i8271_req_t *r = i8271_request(&f);
        if (r) {
            for (unsigned s = 0; s < r->count; s++)
                memset(&f.buf[s * ATOM_DISC_SECTOR_LEN], r->track * 16 + r->sector + s,
                       ATOM_DISC_SECTOR_LEN);
            i8271_served(&f, true, now);
            continue;
        }
        if (f.due == I8271_NEVER) break;
        now = f.due;
        i8271_event(&f, now);
        if (!(f.status & I8271_ST_BUSY)) break;
        if (f.status & I8271_ST_NDR) {
            if (n == 1 && first_gap) *first_gap = now - last;
            last = now;
            uint8_t v = i8271_read(&f, I8271_REG_DATA, now);
            if (n < max) got[n] = v;
            n++;
        }
    }
    return n;
}

static void cmd(uint8_t c, const uint8_t *p, unsigned np) {
    i8271_write(&f, I8271_REG_STATUS, c, now);
    for (unsigned i = 0; i < np; i++) i8271_write(&f, I8271_REG_RESULT, p[i], now);
}

static uint8_t result(void) {
    return i8271_read(&f, I8271_REG_RESULT, now);
}

int main(void) {
    static uint8_t got[4096];
    i8271_init(&f);
    i8271_insert(&f, 0, 40, 1, false);

    /* ---- READ: two sectors, a byte every 64 cycles ------------------- */
    cmd(0x40 | 0x13, (const uint8_t[]){ 3, 4, 0x22 }, 3);
    CHECK(f.status & I8271_ST_BUSY, "busy once the parameters are in");
    uint64_t gap = 0;
    unsigned n = run(got, sizeof got, &gap);
    CHECK(n == 512, "two sectors are 512 bytes: %u", n);
    CHECK(gap == ATOM_FDC_BYTE_CYCLES, "a byte every %u cycles: %llu",
          (unsigned)ATOM_FDC_BYTE_CYCLES, (unsigned long long)gap);
    CHECK(got[0] == 3 * 16 + 4 && got[511] == 3 * 16 + 5, "sectors 4 and 5 of track 3");
    CHECK((f.status & (I8271_ST_RES_FULL | I8271_ST_INT | I8271_ST_BUSY)) ==
          (I8271_ST_RES_FULL | I8271_ST_INT), "a completion raises INT: %02X", f.status);
    CHECK(result() == I8271_OK, "and says OK");
    CHECK(!i8271_int(&f), "reading the result drops INT");
    CHECK(f.drv[0].head == 3 && f.special[I8271_SR_TRACK0] == 3, "the head is on track 3");

    /* ---- past the end of the track: what exists, then not found ---- */
    cmd(0x40 | 0x13, (const uint8_t[]){ 3, 8, 0x25 }, 3);
    n = run(got, sizeof got, NULL);
    CHECK(n == 512, "sectors 8 and 9 exist: %u bytes", n);
    CHECK(result() == I8271_ERR_NOT_FOUND, "sector 10 does not");

    /* ---- the track register moves the head by the difference -------- */
    /* Write the register to 10 while the head is on 3; a seek to 12 then
     * steps two, to physical 5, whose IDs say 5: track 12 is not found. */
    cmd(0x3A, (const uint8_t[]){ I8271_SR_TRACK0, 10 }, 2);
    cmd(0x40 | 0x13, (const uint8_t[]){ 12, 0, 0x21 }, 3);
    n = run(got, sizeof got, NULL);
    CHECK(n == 0 && result() == I8271_ERR_NOT_FOUND, "the IDs carry the physical track");
    CHECK(f.drv[0].head == 5, "two steps from 3: %u", f.drv[0].head);
    /* Track 0 steps out until TRK0, whatever the register said. */
    cmd(0x40 | 0x29, (const uint8_t[]){ 0 }, 1);
    run(got, sizeof got, NULL);
    CHECK(result() == I8271_OK && f.drv[0].head == 0, "SEEK 0 finds track 0");
    cmd(0x40 | 0x2C, NULL, 0);
    uint8_t ds = result();
    CHECK(ds & 0x02u, "TRK0 in the drive status: %02X", ds);
    CHECK((ds & 0x44u) == 0x44u, "both RDY inputs read the selected drive's READY: %02X", ds);

    /* ---- READ ID: C H R N ------------------------------------------- */
    cmd(0x40 | 0x1B, (const uint8_t[]){ 0, 0, 2 }, 3);
    n = run(got, sizeof got, NULL);
    CHECK(n == 8 && got[0] == 0 && got[2] == 0 && got[3] == 1 && got[6] == 1,
          "two IDs, track 0, sectors 0 and 1, 256 bytes");
    CHECK(result() == I8271_OK, "READ ID completes");

    /* ---- VERIFY moves no bytes ------------------------------------------ */
    cmd(0x40 | 0x1F, (const uint8_t[]){ 0, 0, 0x2A }, 3);
    n = run(got, sizeof got, NULL);
    CHECK(n == 0 && result() == I8271_OK, "a whole track verifies");

    /* ---- WRITE and FORMAT, and write protect ------------------------ */
    cmd(0x40 | 0x0B, (const uint8_t[]){ 1, 0, 0x21 }, 3);
    unsigned asked = 0;
    for (int guard = 0; guard < 10000 && !i8271_request(&f); guard++) {
        now = f.due;
        i8271_event(&f, now);
        if (f.status & I8271_ST_NDR) {
            i8271_write(&f, I8271_REG_DATA, (uint8_t)asked, now);
            asked++;
        }
    }
    const i8271_req_t *r = i8271_request(&f);
    CHECK(r && r->op == I8271_REQ_WRITE && r->track == 1 && r->sector == 0 && r->count == 1,
          "a write posts its sector for the port");
    CHECK(asked == 256 && f.buf[255] == 255, "after asking for 256 bytes: %u", asked);
    i8271_served(&f, true, now);
    run(got, sizeof got, NULL);
    CHECK(result() == I8271_OK, "and completes once stored");

    cmd(0x40 | 0x23, (const uint8_t[]){ 2, 0x15, 0x2A, 0, 0x10 }, 5);
    for (int guard = 0; guard < 10000 && !i8271_request(&f); guard++) {
        now = f.due;
        i8271_event(&f, now);
        if (f.status & I8271_ST_NDR) i8271_write(&f, I8271_REG_DATA, 0, now);
    }
    r = i8271_request(&f);
    CHECK(r && r->op == I8271_REQ_WRITE && r->track == 2 && r->count == 10 &&
          f.buf[0] == 0xE5 && f.buf[ATOM_FDC_BUF_LEN - 1] == 0xE5,
          "FORMAT writes the whole track with the filler");
    i8271_served(&f, true, now);
    run(got, sizeof got, NULL);
    CHECK(result() == I8271_OK, "FORMAT completes");

    i8271_insert(&f, 0, 40, 1, true);
    cmd(0x40 | 0x0B, (const uint8_t[]){ 1, 0, 0x21 }, 3);
    n = run(got, sizeof got, NULL);
    CHECK(!i8271_request(&f) && result() == I8271_ERR_PROTECT, "a protected disc refuses");

    /* ---- READY follows the head ------------------------------------- */
    i8271_insert(&f, 0, 40, 1, false);
    cmd(0x35, (const uint8_t[]){ 0x0D, 0x14, 0x05, 0xCA }, 4);   /* the DOS's (#E862) */
    cmd(0x40 | 0x29, (const uint8_t[]){ 0 }, 1);
    run(got, 0, NULL);
    (void)result();
    cmd(0x40 | 0x2C, NULL, 0);
    CHECK(result() & 0x04u, "ready while the head is loaded");
    CHECK(f.due != I8271_NEVER, "the idle count is running");
    uint64_t unload = now + 12u * I8271_REV_CYCLES;
    i8271_event(&f, f.due);
    now = unload;
    cmd(0x40 | 0x2C, NULL, 0);
    CHECK(!(result() & 0x04u), "not ready once twelve idle revolutions unload the head");

    /* A head the DOS loads itself, to wait for READY (#E75B), stays. */
    cmd(0x3A, (const uint8_t[]){ I8271_SR_OUTPUT, 0x48 }, 2);
    i8271_eject(&f, 0);
    i8271_insert(&f, 0, 40, 1, false);
    cmd(0x40 | 0x2C, NULL, 0);
    CHECK(result() & 0x04u, "a disc put in satisfies a DOS waiting for READY");

    /* ---- not ready, and reset --------------------------------------- */
    cmd(0x80 | 0x13, (const uint8_t[]){ 0, 0, 0x21 }, 3);
    run(got, sizeof got, NULL);
    CHECK(result() == I8271_ERR_NOT_READY, "drive 1 is empty");

    cmd(0x40 | 0x13, (const uint8_t[]){ 0, 0, 0x21 }, 3);
    i8271_write(&f, I8271_REG_RESET, 1, now);
    i8271_write(&f, I8271_REG_RESET, 0, now);
    CHECK(f.status == 0 && f.due == I8271_NEVER, "reset drops the command");
    CHECK(f.drv[0].loaded && f.special[I8271_SR_TRACK0] == 0, "and keeps the disc");

    TEST_DONE();
}
