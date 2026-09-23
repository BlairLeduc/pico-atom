/* tapeio.c — tape phase 1's files (tapeio.h). */

#include "tapeio.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "pico/time.h"

#include "ff.h"

#include "config.h"
#include "storage.h"
#include "tape.h"

#define ATOM_DIR "/atom"
#define TAPE_DIR "/atom/tapes"
#define TEMP_ATM TAPE_DIR "/~save.tmp"

/* The card is read and written a sector at a time. */
#define CHUNK 512u

static FIL     s_file;
static uint8_t s_buf[CHUNK];
static char    s_path[ATOM_PATH_MAX];
static char    s_inserted[ATOM_PATH_MAX];

void tapeio_insert(const char *path) {
    s_inserted[0] = 0;
    if (path && strlen(path) < sizeof s_inserted) strcpy(s_inserted, path);
}

const char *tapeio_inserted(void) {
    return s_inserted;
}

/* Neither a directory nor macOS's AppleDouble "._" shadow of a file,
 * which a card written from a Mac carries beside every file. */
static bool is_atm(const FILINFO *fi) {
    if (fi->fattrib & AM_DIR) return false;
    if (fi->fname[0] == '.') return false;
    size_t n = strlen(fi->fname);
    return n > 4 && strcasecmp(fi->fname + n - 4, ".atm") == 0;
}

static bool stem_is(const char *fname, const char *want) {
    size_t n = strlen(fname) - 4;
    return n == strlen(want) && strncasecmp(fname, want, n) == 0;
}

static bool read_header(const char *path, atm_header_t *h) {
    if (f_open(&s_file, path, FA_READ) != FR_OK) return false;
    UINT n = 0;
    FRESULT fr = f_read(&s_file, s_buf, ATM_HEADER_LEN, &n);
    f_close(&s_file);
    if (fr != FR_OK || n != ATM_HEADER_LEN) return false;
    atm_header_decode(s_buf, h);
    return true;
}

/* The file answering to `want`, into s_path. The header name wins over
 * the file name, so a directory of downloads named anything at all still
 * loads by the names the programs inside them ask for. */
static bool find(const char *want, atm_header_t *h) {
    DIR dir;
    static FILINFO fi;
    static char by_stem[ATOM_PATH_MAX];
    by_stem[0] = 0;
    bool found = false;

    if (f_opendir(&dir, TAPE_DIR) != FR_OK) return false;
    while (!found && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (!is_atm(&fi)) continue;
        int n = snprintf(s_path, sizeof s_path, TAPE_DIR "/%s", fi.fname);
        if (n < 0 || (size_t)n >= sizeof s_path) continue;
        atm_header_t fh;
        if (!read_header(s_path, &fh)) continue;
        if (atm_name_matches(&fh, want)) {
            *h = fh;
            found = true;
        } else if (!by_stem[0] && want[0] && stem_is(fi.fname, want)) {
            memcpy(by_stem, s_path, sizeof by_stem);
        }
    }
    f_closedir(&dir);

    if (!found && by_stem[0] && read_header(by_stem, h)) {
        memcpy(s_path, by_stem, sizeof s_path);
        found = true;
    }
    if (!found && !want[0] && s_inserted[0] && read_header(s_inserted, h)) {
        memcpy(s_path, s_inserted, sizeof s_path);
        found = true;
    }
    return found;
}

unsigned tapeio_list(tapeio_entry_t *out, unsigned max) {
    DIR dir;
    static FILINFO fi;
    unsigned n = 0;
    if (f_opendir(&dir, TAPE_DIR) != FR_OK) return 0;
    while (n < max && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (!is_atm(&fi)) continue;
        int len = snprintf(out[n].path, sizeof out[n].path, TAPE_DIR "/%s", fi.fname);
        if (len < 0 || (size_t)len >= sizeof out[n].path) continue;
        if (read_header(out[n].path, &out[n].hdr)) n++;
    }
    f_closedir(&dir);
    return n;
}

static void serve_load(atom_t *m, const char *name) {
    atm_header_t h;
    if (!find(name, &h) || f_open(&s_file, s_path, FA_READ) != FR_OK) {
        printf("  tape         : LOAD \"%s\": no file answers to it; the MOS will ask "
               "for the tape\n", name);
        atom_tape_decline(m);
        return;
    }
    /* A file shorter than its header says is refused before a byte of
     * it reaches the guest, rather than run with the rest missing. */
    if (f_size(&s_file) < (FSIZE_t)ATM_HEADER_LEN + h.len) {
        printf("  tape         : LOAD \"%s\" <- %s: the header says %u bytes and the "
               "file is short; the MOS will ask for the tape\n", name, s_path, h.len);
        f_close(&s_file);
        atom_tape_decline(m);
        return;
    }

    uint32_t t0 = time_us_32();
    uint16_t at = atom_tape_load_begin(m, &h);
    UINT n = 0;
    uint32_t got = 0;
    FRESULT fr = f_lseek(&s_file, ATM_HEADER_LEN);
    while (fr == FR_OK && got < h.len) {
        UINT want = h.len - got < CHUNK ? (UINT)(h.len - got) : CHUNK;
        fr = f_read(&s_file, s_buf, want, &n);
        if (fr != FR_OK || n == 0) break;
        atom_tape_load_data(m, s_buf, n);
        got += n;
    }
    f_close(&s_file);

    /* The card failed part-way: what arrived stays, as it would from a
     * tape that stopped, and the call is declined, so the MOS asks for
     * the tape as though there were no trap (§11.2). */
    if (got != h.len) {
        printf("  tape         : LOAD \"%s\" <- %s: read failed after %lu of %u bytes "
               "(FatFs %d); the MOS will ask for the tape\n", name, s_path,
               (unsigned long)got, h.len, (int)fr);
        atom_tape_decline(m);
        return;
    }
    atom_tape_load_end(m);

    printf("  tape         : LOAD \"%s\" <- %s: %u bytes at #%04X, exec #%04X, %lu us\n",
           name, s_path, h.len, at, h.exec, (unsigned long)(time_us_32() - t0));
}

/* <name>.atm, with anything FAT or a shell would trip on made '_'. */
static void new_path(const char *name) {
    char stem[TAPE_NAME_MAX + 1];
    size_t n = 0;
    for (const char *p = name; *p && n < TAPE_NAME_MAX; p++) {
        unsigned char c = (unsigned char)*p;
        stem[n++] = (isalnum(c) || c == '-') ? (char)c : '_';
    }
    stem[n] = 0;
    snprintf(s_path, sizeof s_path, TAPE_DIR "/%s.atm", n ? stem : "NONAME");
}

static FRESULT write_all(const void *p, UINT len) {
    UINT n = 0;
    FRESULT fr = f_write(&s_file, p, len, &n);
    return (fr == FR_OK && n != len) ? FR_DISK_ERR : fr;
}

static void serve_save(atom_t *m, const char *name) {
    uint32_t t0 = time_us_32();
    atm_header_t h;
    atom_tape_save_header(m, &h);

    (void)f_mkdir(ATOM_DIR);
    (void)f_mkdir(TAPE_DIR);

    /* A file already answering to this name is the one being rewritten,
     * whatever it is called. */
    atm_header_t old;
    if (!find(name, &old) || !atm_name_matches(&old, name)) new_path(name);

    /* Through a temporary file, so a failed write leaves the old one. */
    FRESULT fr = f_open(&s_file, TEMP_ATM, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr == FR_OK) {
        atm_header_encode(&h, s_buf);
        fr = write_all(s_buf, ATM_HEADER_LEN);
        for (uint32_t off = 0; fr == FR_OK && off < h.len; off += CHUNK) {
            size_t n = atom_tape_save_data(m, off, s_buf, CHUNK);
            fr = write_all(s_buf, (UINT)n);
        }
        FRESULT fc = f_close(&s_file);
        if (fr == FR_OK) fr = fc;
    }
    if (fr == FR_OK) {
        (void)f_unlink(s_path);
        fr = f_rename(TEMP_ATM, s_path);
    }

    /* The guest is told nothing either way: OSSAVE has no error return,
     * and a declined save would wait for ever on a recorder that is not
     * there. The log says what happened. */
    atom_tape_save_end(m);
    if (fr == FR_OK) {
        printf("  tape         : SAVE \"%s\" -> %s: %u bytes from #%04X, exec #%04X, %lu us\n",
               name, s_path, h.len, h.load, h.exec, (unsigned long)(time_us_32() - t0));
    } else {
        printf("  tape         : SAVE \"%s\" -> %s FAILED (FatFs error %d); nothing "
               "was written\n", name, s_path, (int)fr);
    }
}

void tapeio_serve(atom_t *m) {
    const tape_t *t = atom_tape_pending(m);
    if (!t) return;

    /* The name outlives the request, which completing clears. */
    char name[TAPE_NAME_MAX + 1];
    memcpy(name, t->name, sizeof name);

    int err = storage_mount();
    if (err != 0) {
        printf("  tape         : %s \"%s\": no card (FatFs error %d)\n",
               t->op == TAPE_LOAD ? "LOAD" : "SAVE", name, err);
        /* A save with nowhere to go still has to return (serve_save). */
        if (t->op == TAPE_SAVE) atom_tape_save_end(m);
        else atom_tape_decline(m);
        return;
    }
    if (t->op == TAPE_LOAD) serve_load(m, name);
    else serve_save(m, name);
    storage_unmount();
}
