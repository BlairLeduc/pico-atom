/* discio.c — disc images on the card (discio.h). */

#include "discio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/time.h"

#include "ff.h"

#include "storage.h"

#define DISC_DIR    "/atom/discs"
#define TRACK_BYTES (ATOM_DISC_SECTORS * ATOM_DISC_SECTOR_LEN)

static FIL s_file;

static struct {
    char    path[ATOM_PATH_MAX];
    uint8_t sides;
} s_drive[ATOM_FDC_DRIVES];

/* Neither a directory nor macOS's AppleDouble "._" shadow of a file. */
static bool has_ext(const char *fname, const char *ext) {
    if (fname[0] == '.') return false;
    size_t n = strlen(fname), k = strlen(ext);
    return n > k && strcasecmp(fname + n - k, ext) == 0;
}

static bool is_disc(const char *fname) {
    return has_ext(fname, ".ssd") || has_ext(fname, ".dsk") || has_ext(fname, ".40t") ||
           has_ext(fname, ".dsd");
}

unsigned discio_list(discio_entry_t *out, unsigned max) {
    DIR dir;
    static FILINFO fi;
    unsigned n = 0;
    if (f_opendir(&dir, DISC_DIR) != FR_OK) return 0;
    while (n < max && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if ((fi.fattrib & AM_DIR) || !is_disc(fi.fname)) continue;
        discio_entry_t *e = &out[n];
        int len = snprintf(e->path, sizeof e->path, DISC_DIR "/%s", fi.fname);
        if (len < 0 || (size_t)len >= sizeof e->path) continue;
        snprintf(e->name, sizeof e->name, "%s", fi.fname);
        e->size = (uint32_t)fi.fsize;
        e->protect = (fi.fattrib & AM_RDO) != 0;
        n++;
    }
    f_closedir(&dir);
    return n;
}

const char *discio_insert(atom_t *m, unsigned drive, const char *path) {
    if (drive >= ATOM_FDC_DRIVES) return "NO SUCH DRIVE";
    s_drive[drive].path[0] = 0;
    atom_disc_eject(m, drive);
    if (!path || !path[0]) return NULL;
    if (strlen(path) >= sizeof s_drive[drive].path) return "NAME TOO LONG";

    static FILINFO fi;
    if (f_stat(path, &fi) != FR_OK) return "CANNOT OPEN";
    uint8_t sides = has_ext(path, ".dsd") ? 2u : 1u;
    uint32_t side_bytes = (uint32_t)fi.fsize / sides;
    if (side_bytes > ATOM_DISC_TRACKS_MAX * TRACK_BYTES) return "TOO BIG";
    /* A short image is a 40-track disc cut off early, unless it is too
     * long to be one. */
    uint8_t tracks = side_bytes > 40u * TRACK_BYTES ? ATOM_DISC_TRACKS_MAX : 40u;
    bool protect = (fi.fattrib & AM_RDO) != 0;

    strcpy(s_drive[drive].path, path);
    s_drive[drive].sides = sides;
    atom_disc_insert(m, drive, tracks, sides, protect);
    printf("  disc         : drive %u: %s, %u tracks, %u side%s%s\n", drive, path,
           tracks, sides, sides > 1 ? "s" : "", protect ? ", protected" : "");
    return NULL;
}

const char *discio_inserted(unsigned drive) {
    return drive < ATOM_FDC_DRIVES ? s_drive[drive].path : "";
}

static bool transfer(const i8271_req_t *r, uint8_t *buf) {
    unsigned sides = s_drive[r->drive].sides;
    FSIZE_t at = ((FSIZE_t)(r->track * sides + r->side) * ATOM_DISC_SECTORS + r->sector) *
                 ATOM_DISC_SECTOR_LEN;
    UINT n = (UINT)r->count * ATOM_DISC_SECTOR_LEN, done = 0;
    bool reading = r->op == I8271_REQ_READ;

    if (f_open(&s_file, s_drive[r->drive].path,
               reading ? FA_READ : (FA_READ | FA_WRITE)) != FR_OK) return false;
    bool ok;
    if (reading) {
        /* Past the end of a short image is unwritten disc: zeros. */
        memset(buf, 0, n);
        ok = at >= f_size(&s_file) ||
             (f_lseek(&s_file, at) == FR_OK && f_read(&s_file, buf, n, &done) == FR_OK);
    } else {
        /* A seek past the end grows the file, as a write there must. */
        ok = f_lseek(&s_file, at) == FR_OK && f_write(&s_file, buf, n, &done) == FR_OK &&
             done == n;
    }
    return f_close(&s_file) == FR_OK && ok;
}

bool discio_serve(atom_t *m) {
    const i8271_req_t *r = atom_disc_request(m);
    if (!r) return true;
    uint32_t t0 = time_us_32();
    bool ok = r->drive < ATOM_FDC_DRIVES && s_drive[r->drive].path[0] &&
              storage_mount() == 0;
    if (ok) {
        ok = transfer(r, m->fdc.buf);
        storage_unmount();
    }
    printf("  disc         : %s drive %u side %u track %u sector %u x%u: %s, %lu us\n",
           r->op == I8271_REQ_READ ? "read" : "write", r->drive, r->side, r->track,
           r->sector, r->count, ok ? "ok" : "FAILED",
           (unsigned long)(time_us_32() - t0));
    atom_disc_served(m, ok);
    return ok;
}
