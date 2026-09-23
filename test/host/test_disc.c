/* test_disc.c — AtomDOS and the 8271, run by the real DOS ROM
 * (design.md §11.4, §17 M9).
 *
 * The reference is the DOS itself: *DOS, then *CAT, *LOAD, *RUN and
 * *SAVE typed at it as the Atom Disc Pack manual has them, against a disc image built here and served the
 * way main.c serves one, a request at a time after each field. What the
 * DOS prints, what lands in RAM and what lands on the image are all
 * checked against the image's own bytes.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bus.h"
#include "guest.h"
#include "snapshot.h"
#include "test_util.h"

#define TRACKS   40u
#define SECTORS  ATOM_DISC_SECTORS
#define SECLEN   ATOM_DISC_SECTOR_LEN
#define IMG_LEN  (TRACKS * SECTORS * SECLEN)

static guest_t g;
static uint8_t disc[ATOM_FDC_DRIVES][IMG_LEN];
static unsigned requests, reads, writes;

/* ---- the port, as main.c and discio.c do it ------------------------ */

static void serve(atom_t *m) {
    int16_t discard[ATOM_AUDIO_BUF_LEN];
    (void)atom_audio_drain(m, discard, ATOM_AUDIO_BUF_LEN);

    const i8271_req_t *r = atom_disc_request(m);
    if (!r) return;
    requests++;
    size_t at = ((size_t)r->track * SECTORS + r->sector) * SECLEN;
    size_t n = (size_t)r->count * SECLEN;
    if (r->side != 0 || at + n > IMG_LEN) {
        atom_disc_served(m, false);
        return;
    }
    if (r->op == I8271_REQ_READ) {
        memcpy(m->fdc.buf, &disc[r->drive][at], n);
        reads += r->count;
    } else {
        memcpy(&disc[r->drive][at], m->fdc.buf, n);
        writes += r->count;
    }
    atom_disc_served(m, true);
}

/* ---- an Acorn catalogue: sector 0 names, sector 1 addresses -------- */

static void put_title(uint8_t *img, const char *title) {
    memset(img, ' ', 8);
    memset(img + SECLEN, ' ', 4);
    for (int i = 0; title[i] && i < 12; i++) {
        if (i < 8) img[i] = (uint8_t)title[i];
        else       img[SECLEN + i - 8] = (uint8_t)title[i];
    }
    img[SECLEN + 5] = 0;
    img[SECLEN + 6] = (uint8_t)((TRACKS * SECTORS) >> 8);
    img[SECLEN + 7] = (uint8_t)(TRACKS * SECTORS);
}

/* Files are listed from the highest start sector down, so each new one,
 * being the highest, goes in at the front. */
static void add_file(uint8_t *img, const char *name, uint16_t load, uint16_t exec,
                     const uint8_t *data, uint16_t len, uint16_t start) {
    uint8_t n = img[SECLEN + 5] / 8u;
    memmove(img + 16, img + 8, (size_t)n * 8u);
    memmove(img + SECLEN + 16, img + SECLEN + 8, (size_t)n * 8u);
    memset(img + 8, ' ', 8);
    memcpy(img + 8, name, strlen(name));
    uint8_t *e = img + SECLEN + 8;
    e[0] = (uint8_t)load; e[1] = (uint8_t)(load >> 8);
    e[2] = (uint8_t)exec; e[3] = (uint8_t)(exec >> 8);
    e[4] = (uint8_t)len;  e[5] = (uint8_t)(len >> 8);
    e[6] = (uint8_t)((start >> 8) & 3u);
    e[7] = (uint8_t)start;
    img[SECLEN + 5] = (uint8_t)((n + 1u) * 8u);
    memcpy(img + (size_t)start * SECLEN, data, len);
}

/* A catalogue entry by name, or NULL. */
static const uint8_t *find(const uint8_t *img, const char *name, uint16_t *start, uint16_t *len,
                           uint16_t *load) {
    uint8_t n = img[SECLEN + 5] / 8u;
    for (uint8_t i = 0; i < n; i++) {
        const uint8_t *nm = img + 8 + i * 8;
        size_t k = strlen(name);
        if (memcmp(nm, name, k) != 0 || (k < 7 && nm[k] != ' ')) continue;
        const uint8_t *e = img + SECLEN + 8 + i * 8;
        *load = (uint16_t)(e[0] | (e[1] << 8));
        *len = (uint16_t)(e[4] | (e[5] << 8));
        *start = (uint16_t)(e[7] | ((e[6] & 3u) << 8));
        return img + (size_t)*start * SECLEN;
    }
    return NULL;
}

/* ---- the screen ------------------------------------------------------ */

static bool screen_has(const char *s) {
    for (int r = 0; r < 16; r++) {
        if (strstr(guest_row(&g.m, r), s)) return true;
    }
    return false;
}

static int screen_count(const char *s) {
    int n = 0;
    for (int r = 0; r < 16; r++) n += strstr(guest_row(&g.m, r), s) != NULL;
    return n;
}

static void dump_screen(void) {
    for (int r = 0; r < 16; r++) fprintf(stderr, "    |%s\n", guest_row(&g.m, r));
}

/* Type a command and give the disc time: a track is 12 fields. */
static void command(const char *s) {
    guest_type(&g, s);
    guest_fields(&g, 150);
    if (getenv("DUMP")) dump_screen();
}

/* ---- a snapshot in memory ------------------------------------------- */

static uint8_t snap[SNAP_FILE_LEN];
static size_t  snap_at;

static bool snap_put(void *ctx, const uint8_t *src, size_t n) {
    (void)ctx;
    if (snap_at + n > sizeof snap) return false;
    memcpy(snap + snap_at, src, n);
    snap_at += n;
    return true;
}

static bool snap_get(void *ctx, uint8_t *dst, size_t n) {
    (void)ctx;
    if (snap_at + n > sizeof snap) return false;
    memcpy(dst, snap + snap_at, n);
    snap_at += n;
    return true;
}

/* A program that prints DISC OK through OSWRCH and returns. It lives
 * clear of #2900: that is BASIC's text, and BASIC walks it for its end
 * after any error (#CDA4), for ever if a page of it holds no CR. */
static const uint8_t hello[] = {
    0xA2, 0x00,             /* 3C00 LDX #0        */
    0xBD, 0x0E, 0x3C,       /* 3C02 LDA #3C0E,X   */
    0xF0, 0x06,             /* 3C05 BEQ #3C0D     */
    0x20, 0xF4, 0xFF,       /* 3C07 JSR #FFF4     */
    0xE8,                   /* 3C0A INX           */
    0xD0, 0xF5,             /* 3C0B BNE #3C02     */
    0x60,                   /* 3C0D RTS           */
    'D', 'I', 'S', 'C', ' ', 'O', 'K', 0x0D, 0x0A, 0x00,
};

int main(void) {
    const char *dir;
    if (!guest_find_roms(&dir) || !guest_have_rom(ROM_DOS)) {
        printf("skip: the kernel, BASIC and DOS ROMs are not in %s\n", dir);
        return TEST_SKIP_CODE;
    }

    /* The disc: HELLO in the middle of track 0; DATA from sector 8 of
     * track 0 into track 1, so that one load is two of the DOS's
     * per-track reads. */
    static uint8_t data[1500];
    for (size_t i = 0; i < sizeof data; i++) data[i] = (uint8_t)(i * 7u + (i >> 8));
    put_title(disc[0], "PICOATOM");
    add_file(disc[0], "HELLO", 0x3C00, 0x3C00, hello, sizeof hello, 2);
    add_file(disc[0], "DATA", 0x3000, 0x3000, data, sizeof data, 8);

    guest_boot(&g);
    g.on_field = serve;
    atom_disc_insert(&g.m, 0, TRACKS, 1, false);

    /* ---- *DOS: the kernel's own command for #E000 (#F8E7), and the
     * DOS takes over the vectors -------------------------------------- */
    command("*DOS\n");
    CHECK(g.m.ram[0x0200] == 0x7B && g.m.ram[0x0201] == 0xE8,
          "the DOS points NMI at its handler, #E87B (#EEEF)");
    CHECK(g.m.fdc.special[I8271_SR_MODE] == 0xC1,
          "the DOS puts the 8271 in non-DMA mode: mode register %02X",
          g.m.fdc.special[I8271_SR_MODE]);

    /* A snapshot now carries the DOS's setup of the chip. It is taken
     * here and restored at the end, into a machine that has been reset
     * and never linked the DOS. */
    snap_at = 0;
    CHECK(snapshot_save(&g.m, snap_put, NULL) == SNAP_OK, "a snapshot with the DOS linked");

    /* ---- *CAT: the catalogue, read off track 0 --------------------- */
    command("*CAT\n");
    CHECK(screen_has("PICOATOM"), "*CAT shows the disc title");
    CHECK(screen_has("HELLO") && screen_has("DATA"), "*CAT lists both files");
    if (test_failures) dump_screen();
    CHECK(reads >= 2, "the catalogue is two sectors off the image: %u read", reads);

    /* ---- *LOAD across a track boundary ----------------------------- */
    memset(&g.m.ram[0x3000], 0, sizeof data);
    unsigned before = requests;
    command("*LOAD DATA\n");
    CHECK(memcmp(&g.m.ram[0x3000], data, sizeof data) == 0,
          "*LOAD puts the file's bytes at its load address");
    CHECK(requests - before >= 2, "a file across a track is more than one request: %u",
          requests - before);
    CHECK(g.m.cpu.undoc_count == 0, "no undocumented opcodes along the way");

    /* ---- *RUN: a program off the disc runs -------------------------- */
    command("*RUN HELLO\n");
    CHECK(screen_has("DISC OK"), "*RUN loads HELLO and runs it");
    if (test_failures) dump_screen();

    /* ---- *SAVE: bytes out of RAM onto the image --------------------- */
    for (unsigned i = 0; i < 0x300; i++) g.m.ram[0x3800 + i] = (uint8_t)(0xA5 ^ i ^ (i >> 8));
    before = writes;
    command("*SAVE\"OUT\" 3800 3B00\n");
    uint16_t start, len, load;
    const uint8_t *out = find(disc[0], "OUT", &start, &len, &load);
    CHECK(out != NULL, "*SAVE adds OUT to the catalogue");
    if (out) {
        CHECK(load == 0x3800 && len == 0x300, "OUT at #%04X, %u bytes", load, len);
        CHECK(memcmp(out, &g.m.ram[0x3800], 0x300) == 0, "OUT holds the bytes saved");
    }
    CHECK(writes - before >= 3, "*SAVE writes the data and the catalogue: %u sectors",
          writes - before);
    if (test_failures) dump_screen();

    /* ---- write protect: the DOS says PROT (#E7A9) ------------------ */
    atom_disc_insert(&g.m, 0, TRACKS, 1, true);
    uint8_t copy[2 * SECLEN];
    memcpy(copy, disc[0], sizeof copy);
    command("*SAVE\"NOPE\" 3800 3900\n");
    CHECK(screen_has("PROT"), "a protected disc answers PROT");
    CHECK(memcmp(copy, disc[0], sizeof copy) == 0, "and its catalogue is untouched");
    if (test_failures) dump_screen();

    /* ---- drive 1, and drive 2 which is drive 0's other side ------- */
    atom_disc_insert(&g.m, 0, TRACKS, 1, false);
    put_title(disc[1], "SECOND");
    add_file(disc[1], "TWO", 0x3C00, 0x3C00, hello, sizeof hello, 2);
    atom_disc_insert(&g.m, 1, TRACKS, 1, false);
    command("*DRIVE 1\n");
    command("*CAT\n");
    CHECK(screen_has("SECOND") && screen_has("TWO"), "*CAT on drive 1 reads drive 1's disc");
    if (test_failures) dump_screen();

    /* A disc changed between commands is noticed: the head unloads with
     * the door, READY drops, and the DOS reads the new catalogue rather
     * than trusting its copy (#E731). */
    static uint8_t second[IMG_LEN];
    memcpy(second, disc[1], IMG_LEN);
    put_title(disc[1], "THIRD");
    atom_disc_eject(&g.m, 1);
    atom_disc_insert(&g.m, 1, TRACKS, 1, false);
    command("*CAT\n");
    CHECK(screen_has("THIRD"), "a changed disc's catalogue is read afresh");
    if (test_failures) dump_screen();
    memcpy(disc[1], second, IMG_LEN);
    atom_disc_eject(&g.m, 1);
    atom_disc_insert(&g.m, 1, TRACKS, 1, false);

    /* A single-sided disc has no side 1: the DOS gives up with an error
     * rather than hanging. */
    command("*DRIVE 2\n");
    command("*CAT\n");
    CHECK(!atom_disc_request(&g.m) && !i8271_busy(&g.m.fdc), "the FDC is idle after side 1");
    CHECK(screen_has("ERROR"), "*CAT on a missing side is an error");
    if (test_failures) dump_screen();

    /* An empty drive is not ready, and the DOS waits for it, polling
     * READ DRIVE STATUS (#E774) as it would on the real machine. A disc
     * put in lets it go on. */
    atom_disc_eject(&g.m, 1);
    command("*DRIVE 1\n");
    command("*CAT\n");
    int shown = screen_count("SECOND");
    CHECK(!screen_has(">*CAT") || shown == 1, "an empty drive has no catalogue");
    atom_disc_insert(&g.m, 1, TRACKS, 1, false);
    guest_fields(&g, 100);
    CHECK(screen_count("SECOND") == shown + 1, "the waiting *CAT reads the disc put in");
    if (test_failures) dump_screen();

    /* ---- snapshots: refused mid-command, and the setup restored ---- */
    /* A READ of drive 0, track 0, sector 0, put to the chip as the DOS
     * would (#E7ED); a snapshot is refused until the result is taken. */
    bus_write(&g.m, 0x0A00, 0x40 | 0x13);
    bus_write(&g.m, 0x0A01, 0);
    bus_write(&g.m, 0x0A01, 0);
    bus_write(&g.m, 0x0A01, 0x21);
    static uint8_t scratch[SNAP_FILE_LEN];
    memcpy(scratch, snap, sizeof snap);
    snap_at = 0;
    CHECK(i8271_busy(&g.m.fdc) && snapshot_save(&g.m, snap_put, NULL) == SNAP_BUSY,
          "no snapshot while the FDC is mid-command");
    memcpy(snap, scratch, sizeof snap);
    guest_fields(&g, 30);
    CHECK(!i8271_busy(&g.m.fdc), "the READ completes");
    (void)bus_read(&g.m, 0x0A01);

    atom_reset(&g.m);
    guest_fields(&g, 120);
    g.m.fdc.special[I8271_SR_MODE] = 0;
    snap_at = 0;
    CHECK(snapshot_load(&g.m, snap_get, NULL) == SNAP_OK, "the snapshot restores");
    CHECK(g.m.fdc.special[I8271_SR_MODE] == 0xC1, "with the chip in non-DMA mode");
    command("*DRIVE 0\n");
    command("*RUN\"HELLO\"\n");
    CHECK(screen_count("DISC OK") >= 1, "and the DOS reads the disc after it");
    if (test_failures) dump_screen();

    /* ---- a real disc, when one is named: PICO_ATOM_DISC=games1.40t --- */
    const char *real = getenv("PICO_ATOM_DISC");
    const char *prog = getenv("PICO_ATOM_DISC_RUN");
    if (real) {
        FILE *fp = fopen(real, "rb");
        CHECK(fp != NULL, "cannot open %s", real);
        if (fp) {
            memset(disc[0], 0, IMG_LEN);
            size_t got = fread(disc[0], 1, IMG_LEN, fp);
            fclose(fp);
            printf("  %s: %zu bytes\n", real, got);
            atom_disc_insert(&g.m, 0, TRACKS, 1, false);
            command("*DRIVE 0\n");
            command("*CAT\n");
            dump_screen();
            if (prog) {
                /* A BASIC program: loaded by BASIC's LOAD, which the DOS
                 * has taken over, then RUN. */
                char line[40];
                snprintf(line, sizeof line, "LOAD\"%s\"\n", prog);
                command(line);
                command("RUN\n");
                guest_fields(&g, 300);
                dump_screen();
                CHECK(g.m.cpu.undoc_count == 0, "no undocumented opcodes running %s", prog);
            }
        }
    }

    TEST_DONE();
}
