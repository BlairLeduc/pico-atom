/* test_snapshot.c — whole-machine snapshots (design.md §11.5).
 *
 * The property that matters is executed, not compared: a machine saved
 * part-way through a program, restored into a fresh machine and run on,
 * must arrive exactly where the original arrives. Then the refusals —
 * a damaged file, other ROMs, another memory map — each leave the
 * machine they were refused by untouched.
 */

#include <stdio.h>
#include <string.h>

#include "guest.h"
#include "snapshot.h"
#include "test_util.h"

/* ---- a stream in memory --------------------------------------------- */

typedef struct {
    uint8_t buf[SNAP_FILE_LEN + 16];
    size_t  len, pos;
    size_t  fail_at;     /* fail the call that would cross this; 0 never */
} mem_t;

static bool mem_write(void *ctx, const uint8_t *src, size_t n) {
    mem_t *s = ctx;
    if (s->len + n > sizeof s->buf) return false;
    if (s->fail_at && s->len + n > s->fail_at) return false;
    memcpy(s->buf + s->len, src, n);
    s->len += n;
    return true;
}

static bool mem_read(void *ctx, uint8_t *dst, size_t n) {
    mem_t *s = ctx;
    if (s->pos + n > s->len) return false;
    memcpy(dst, s->buf + s->pos, n);
    s->pos += n;
    return true;
}

static mem_t snap;

static snap_status_t check(const atom_t *m) { snap.pos = 0; return snapshot_check(m, mem_read, &snap); }
static snap_status_t load(atom_t *m)        { snap.pos = 0; return snapshot_load(m, mem_read, &snap); }

/* Everything a program can see, and the CPU. */
static bool same(const atom_t *a, const atom_t *b, const char *what) {
    if (memcmp(a->ram, b->ram, sizeof a->ram) != 0) {
        for (unsigned i = 0; i < sizeof a->ram; i++) {
            if (a->ram[i] != b->ram[i]) {
                fprintf(stderr, "    %s: #%04X %02X vs %02X\n", what, i, a->ram[i], b->ram[i]);
                break;
            }
        }
        return false;
    }
    const m6502_t *x = &a->cpu, *y = &b->cpu;
    if (x->pc != y->pc || x->a != y->a || x->x != y->x || x->y != y->y ||
        x->s != y->s || x->p != y->p || x->cycles != y->cycles) {
        fprintf(stderr, "    %s: CPU PC %04X/%04X cycles %llu/%llu\n", what, x->pc, y->pc,
                (unsigned long long)x->cycles, (unsigned long long)y->cycles);
        return false;
    }
    return memcmp(&a->via, &b->via, sizeof a->via) == 0 &&
           memcmp(&a->ppi, &b->ppi, sizeof a->ppi) == 0;
}

static guest_t g, h;

int main(void) {
    /* The CRC is zlib's. */
    CHECK(snapshot_crc32(0, (const uint8_t *)"123456789", 9) == 0xCBF43926u,
          "CRC-32 of 123456789 is %08X", (unsigned)snapshot_crc32(0, (const uint8_t *)"123456789", 9));

    const char *dir;
    if (!guest_find_roms(&dir)) {
        printf("skipped: no kernel and BASIC images in %s\n", dir);
        return TEST_SKIP_CODE;
    }

    /* A program that keeps the whole machine busy: the screen, the
     * speaker, the keyboard scan, the VIA's timer through the MOS, and
     * the arithmetic ROM if it is fitted. */
    guest_boot(&g);
    guest_type(&g, "10 CLEAR 0\n20 FOR I=0 TO 63;PLOT 13,I,(I*I)%48;N.\n"
                   "30 P.$7;A=A+1;GOTO 20\nRUN\n");
    guest_fields(&g, 37);

    snap.len = 0;
    CHECK(snapshot_save(&g.m, mem_write, &snap) == SNAP_OK, "save");
    CHECK(snap.len == SNAP_FILE_LEN, "a snapshot is %zu bytes, want %u", snap.len,
          (unsigned)SNAP_FILE_LEN);

    /* ROM bytes are not in the file. */
    {
        const uint8_t *space = snap.buf + SNAP_HEADER_LEN + SNAP_STATE_LEN;
        bool zero = true;
        for (unsigned a = 0xC000; a < 0x10000; a++) zero = zero && space[a] == 0;
        CHECK(zero, "ROM pages must be written as zeros");
        CHECK(memcmp(space, g.m.ram, 0x8000) == 0, "RAM is written as it stands");
    }

    /* Run on, and restore into a machine that has been doing something
     * else entirely: they must meet. */
    static atom_t ahead;
    guest_fields(&g, 150);
    atom_copy(&ahead, &g.m);

    guest_boot(&h);
    guest_type(&h, "PRINT 1\n");
    CHECK(check(&h.m) == SNAP_OK, "check: %s", snapshot_status_str(check(&h.m)));
    CHECK(load(&h.m) == SNAP_OK, "load");
    keymatrix_init(&h.k);
    guest_fields(&h, 150);
    CHECK(same(&ahead, &h.m, "resumed"), "a restored machine should run to the same state");
    CHECK(h.m.cpu.undoc_count == 0, "undocumented opcode after restore");

    /* Damage: one byte anywhere in the payload. */
    static atom_t before;
    atom_copy(&before, &h.m);
    snap.buf[SNAP_HEADER_LEN + SNAP_STATE_LEN + 0x2950] ^= 0x01u;
    CHECK(check(&h.m) == SNAP_CORRUPT, "a flipped bit: %s", snapshot_status_str(check(&h.m)));
    CHECK(same(&before, &h.m, "untouched"), "a refused check changes nothing");
    snap.buf[SNAP_HEADER_LEN + SNAP_STATE_LEN + 0x2950] ^= 0x01u;

    /* Truncated, as a card pulled mid-write leaves it. */
    size_t full = snap.len;
    snap.len = full - 1000;
    CHECK(check(&h.m) == SNAP_IO, "truncated: %s", snapshot_status_str(check(&h.m)));
    snap.len = full;

    /* Not a snapshot at all, and one from a later format. */
    snap.buf[0] = 'X';
    CHECK(check(&h.m) == SNAP_NOT_SNAPSHOT, "magic: %s", snapshot_status_str(check(&h.m)));
    snap.buf[0] = 'P';
    snap.buf[8] = SNAP_VERSION + 1;
    CHECK(check(&h.m) == SNAP_NEWER, "version: %s", snapshot_status_str(check(&h.m)));
    CHECK(load(&h.m) == SNAP_NEWER, "load refuses a newer version too");
    snap.buf[8] = SNAP_VERSION;

    /* Other ROMs: a machine whose BASIC differs by one byte. */
    h.m.ram[0xC123] ^= 0xFFu;
    CHECK(check(&h.m) == SNAP_OTHER_ROMS, "other ROMs: %s", snapshot_status_str(check(&h.m)));
    CHECK(load(&h.m) == SNAP_OTHER_ROMS, "load refuses other ROMs");
    h.m.ram[0xC123] ^= 0xFFu;

    /* Another memory map. */
    h.m.cfg.video_aperture = true;
    CHECK(check(&h.m) == SNAP_OTHER_MACHINE, "other map: %s", snapshot_status_str(check(&h.m)));
    h.m.cfg.video_aperture = false;
    CHECK(same(&before, &h.m, "untouched"), "refusals change nothing");

    /* A write that fails part-way reports it. */
    mem_t *s = &snap;
    s->len = 0;
    s->fail_at = 5000;
    CHECK(snapshot_save(&g.m, mem_write, s) == SNAP_IO, "a failed write is reported");
    s->fail_at = 0;

    /* Not while the CPU is stalled on a tape call. */
    g.m.tape.op = TAPE_LOAD;
    s->len = 0;
    CHECK(snapshot_save(&g.m, mem_write, s) == SNAP_BUSY, "busy");
    g.m.tape.op = TAPE_NONE;

    TEST_DONE();
}
