/* snapio.c — snapshot slots on the card (snapio.h). */

#include "snapio.h"

#include <stdio.h>

#include "ff.h"

#define SNAP_DIR "/atom/snaps"

static FIL s_file;

static void path(char *out, size_t cap, unsigned slot, const char *ext) {
    snprintf(out, cap, SNAP_DIR "/slot%u.%s", slot + 1u, ext);
}

static bool fwrite_cb(void *ctx, const uint8_t *src, size_t n) {
    UINT w = 0;
    return f_write((FIL *)ctx, src, (UINT)n, &w) == FR_OK && w == n;
}

static bool fread_cb(void *ctx, uint8_t *dst, size_t n) {
    UINT r = 0;
    return f_read((FIL *)ctx, dst, (UINT)n, &r) == FR_OK && r == n;
}

snap_status_t snapio_save(const atom_t *m, unsigned slot) {
    char tmp[40], dst[40];
    path(tmp, sizeof tmp, slot, "new");
    path(dst, sizeof dst, slot, "psnap");
    (void)f_mkdir("/atom");
    (void)f_mkdir(SNAP_DIR);

    if (f_open(&s_file, tmp, FA_WRITE | FA_CREATE_ALWAYS) != FR_OK) return SNAP_IO;
    snap_status_t st = snapshot_save(m, fwrite_cb, &s_file);
    FRESULT fc = f_close(&s_file);
    if (st == SNAP_OK && fc != FR_OK) st = SNAP_IO;
    if (st != SNAP_OK) {
        (void)f_unlink(tmp);
        return st;
    }
    /* The publish. Between the unlink and the rename only slotN.new
     * exists, and snapio_load takes it. */
    (void)f_unlink(dst);
    return f_rename(tmp, dst) == FR_OK ? SNAP_OK : SNAP_IO;
}

/* Two passes over one file: check, then load (snapshot.h). */
static snap_status_t check_file(const atom_t *m, const char *p) {
    if (f_open(&s_file, p, FA_READ) != FR_OK) return SNAP_IO;
    snap_status_t st = snapshot_check(m, fread_cb, &s_file);
    f_close(&s_file);
    return st;
}

static snap_status_t load_file(atom_t *m, const char *p) {
    if (f_open(&s_file, p, FA_READ) != FR_OK) return SNAP_IO;
    snap_status_t st = snapshot_load(m, fread_cb, &s_file);
    f_close(&s_file);
    return st;
}

snap_status_t snapio_load(atom_t *m, unsigned slot, bool *recovered) {
    char main_path[40], tmp[40];
    path(main_path, sizeof main_path, slot, "psnap");
    path(tmp, sizeof tmp, slot, "new");
    *recovered = false;

    snap_status_t st = check_file(m, main_path);
    if (st == SNAP_OK) return load_file(m, main_path);

    /* Missing or damaged: an interrupted publish leaves a whole .new. A
     * snapshot that is whole but from another machine is not damage, and
     * is reported as it is. */
    if (st == SNAP_IO || st == SNAP_CORRUPT || st == SNAP_NOT_SNAPSHOT) {
        if (check_file(m, tmp) == SNAP_OK) {
            *recovered = true;
            return load_file(m, tmp);
        }
    }
    return st;
}

bool snapio_exists(unsigned slot) {
    char p[40];
    static FILINFO fi;   /* 270 bytes with long names: not on the stack */
    path(p, sizeof p, slot, "psnap");
    if (f_stat(p, &fi) == FR_OK) return true;
    path(p, sizeof p, slot, "new");
    return f_stat(p, &fi) == FR_OK;
}

bool snapio_delete(unsigned slot) {
    char p[40];
    path(p, sizeof p, slot, "psnap");
    FRESULT a = f_unlink(p);
    path(p, sizeof p, slot, "new");
    FRESULT b = f_unlink(p);
    return a == FR_OK || b == FR_OK;
}
