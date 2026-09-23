/* keymapio.c — game keymaps off the card (keymapio.h, design.md §10.5). */

#include "keymapio.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ff.h"

#include "config.h"

#define KEYMAP_DIR "/atom/keymaps"

static keylayout_t s_card[ATOM_KEYMAP_LAYOUTS];
static unsigned    s_n_card;
static char        s_error[40];
static char        s_text[ATOM_KEYMAP_FILE_MAX];
static FIL         s_file;

/* Not a directory, nor macOS's "._" shadow of a file. */
static bool is_map(const FILINFO *fi) {
    if (fi->fattrib & AM_DIR) return false;
    if (fi->fname[0] == '.') return false;
    size_t n = strlen(fi->fname);
    return n > 4 && strcasecmp(fi->fname + n - 4, ".map") == 0;
}

static void fail(const char *fname, unsigned line, const char *why) {
    printf("  keymaps      : %s/%s line %u: %s; left out\n", KEYMAP_DIR, fname, line, why);
    if (s_error[0]) return;
    if (line) snprintf(s_error, sizeof s_error, "%.12s %u: %s", fname, line, why);
    else snprintf(s_error, sizeof s_error, "%.12s: %s", fname, why);
}

/* One file into s_card[s_n_card]. */
static bool load(const char *fname) {
    char path[ATOM_PATH_MAX];
    int n = snprintf(path, sizeof path, KEYMAP_DIR "/%s", fname);
    if (n < 0 || (size_t)n >= sizeof path) return false;

    if (f_open(&s_file, path, FA_READ) != FR_OK) {
        fail(fname, 0, "cannot open");
        return false;
    }
    UINT got = 0;
    bool big = f_size(&s_file) > sizeof s_text;
    FRESULT fr = big ? FR_OK : f_read(&s_file, s_text, sizeof s_text, &got);
    f_close(&s_file);
    if (big || fr != FR_OK) {
        fail(fname, 0, big ? "too big" : "cannot read");
        return false;
    }

    /* The file's own name, without ".map", if it gives none. */
    char stem[ATOM_KEYMAP_NAME_LEN + 1];
    size_t len = strlen(fname) - 4;
    if (len > ATOM_KEYMAP_NAME_LEN) len = ATOM_KEYMAP_NAME_LEN;
    memcpy(stem, fname, len);
    stem[len] = 0;

    unsigned line = 0;
    keylayout_status_t st = keylayout_parse(&s_card[s_n_card], stem, s_text, got, &line);
    if (st != KL_OK) {
        fail(fname, line, keylayout_status_str(st));
        return false;
    }
    if (keymapio_find(s_card[s_n_card].name) >= 0) {
        fail(fname, 0, "name taken");
        return false;
    }
    return true;
}

void keymapio_scan(void) {
    s_n_card = 0;
    s_error[0] = 0;

    DIR dir;
    static FILINFO fi;
    unsigned room = ATOM_KEYMAP_LAYOUTS - (unsigned)keylayout_builtin_len;
    if (f_opendir(&dir, KEYMAP_DIR) != FR_OK) return;
    while (f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        if (!is_map(&fi)) continue;
        if (s_n_card >= room) {
            fail(fi.fname, 0, "too many layouts");
            break;
        }
        if (load(fi.fname)) {
            const keylayout_t *l = &s_card[s_n_card++];
            printf("  keymaps      : %s: \"%s\", %u binding(s), %u tape(s)\n", fi.fname,
                   l->name, l->n, l->n_tapes);
        }
    }
    f_closedir(&dir);
}

unsigned keymapio_count(void) {
    return (unsigned)keylayout_builtin_len + s_n_card;
}

const keylayout_t *keymapio_get(unsigned i) {
    if (i < keylayout_builtin_len) return &keylayout_builtin[i];
    i -= (unsigned)keylayout_builtin_len;
    return i < s_n_card ? &s_card[i] : NULL;
}

int keymapio_find(const char *name) {
    for (unsigned i = 0; i < keymapio_count(); i++) {
        if (strcmp(keymapio_get(i)->name, name) == 0) return (int)i;
    }
    return -1;
}

const keylayout_t *keymapio_for_tape(const char *atm_name) {
    for (unsigned i = 0; i < keymapio_count(); i++) {
        if (keylayout_for_tape(keymapio_get(i), atm_name)) return keymapio_get(i);
    }
    return NULL;
}

const char *keymapio_error(void) {
    return s_error;
}
