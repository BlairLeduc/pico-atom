/* settingsio.c — the settings file on the card (settingsio.h, §11.7). */

#include "settingsio.h"

#include <ctype.h>
#include <stdio.h>

#include "ff.h"

static char s_error[40];
static char s_text[ATOM_SETTINGS_FILE_MAX];
static FIL  s_file;

static void keep(const char *msg) {
    if (s_error[0]) return;
    size_t i = 0;
    for (; msg[i] && i + 1 < sizeof s_error; i++) {
        s_error[i] = (char)toupper((unsigned char)msg[i]);
    }
    s_error[i] = 0;
}

void settingsio_fail(const char *what, const char *why) {
    printf("  settings     : %s: %s\n", what, why);
    char msg[sizeof s_error];
    snprintf(msg, sizeof msg, "CFG %s: %s", what, why);
    keep(msg);
}

void settingsio_load(settings_t *out) {
    settings_default(out);
    s_error[0] = 0;

    if (f_open(&s_file, SETTINGSIO_PATH, FA_READ) != FR_OK) {
        printf("  settings     : no %s; defaults\n", SETTINGSIO_PATH);
        return;
    }
    UINT got = 0;
    bool big = f_size(&s_file) > sizeof s_text;
    FRESULT fr = big ? FR_OK : f_read(&s_file, s_text, sizeof s_text, &got);
    f_close(&s_file);
    if (big || fr != FR_OK) {
        settingsio_fail("file", big ? "too big" : "cannot read");
        return;
    }

    unsigned line = 0;
    settings_status_t st = settings_parse(out, s_text, got, &line);
    if (st != SET_OK) {
        char at[16];
        snprintf(at, sizeof at, "line %u", line);
        settingsio_fail(at, settings_status_str(st));
    }
    printf("  settings     : %s read\n", SETTINGSIO_PATH);
}

const char *settingsio_error(void) {
    return s_error;
}
