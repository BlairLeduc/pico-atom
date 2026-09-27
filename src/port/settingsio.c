/* settingsio.c — the settings file on the card (settingsio.h, §11.7). */

#include "settingsio.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

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

/* The file's text into s_text: the file, or the temporary one a save
 * left without its rename. FR_NO_FILE if neither is there; FR_DENIED if
 * it is too big to hold. */
static FRESULT read_text(UINT *got, const char **from) {
    *got = 0;
    *from = SETTINGSIO_PATH;
    FRESULT fr = f_open(&s_file, SETTINGSIO_PATH, FA_READ);
    if (fr == FR_NO_FILE) {
        *from = SETTINGSIO_TEMP;
        fr = f_open(&s_file, SETTINGSIO_TEMP, FA_READ);
    }
    if (fr != FR_OK) return fr;
    bool big = f_size(&s_file) > sizeof s_text;
    fr = big ? FR_DENIED : f_read(&s_file, s_text, sizeof s_text, got);
    f_close(&s_file);
    return fr;
}

void settingsio_load(settings_t *out) {
    settings_default(out);
    s_error[0] = 0;

    UINT got = 0;
    const char *from;
    FRESULT fr = read_text(&got, &from);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        printf("  settings     : no %s; defaults\n", SETTINGSIO_PATH);
        return;
    }
    if (fr != FR_OK) {
        settingsio_fail("file", fr == FR_DENIED ? "too big" : "cannot read");
        return;
    }

    unsigned line = 0;
    settings_status_t st = settings_parse(out, s_text, got, &line);
    if (st != SET_OK) {
        char at[16];
        snprintf(at, sizeof at, "line %u", line);
        settingsio_fail(at, settings_status_str(st));
    }
    printf("  settings     : %s read\n", from);
}

void settingsio_peek(settings_t *out) {
    settings_default(out);
    UINT got = 0;
    const char *from;
    if (read_text(&got, &from) != FR_OK) return;
    unsigned line = 0;
    (void)settings_parse(out, s_text, got, &line);
}

static FRESULT write_all(const char *p, size_t n) {
    UINT w = 0;
    FRESULT fr = f_write(&s_file, p, (UINT)n, &w);
    return fr == FR_OK && w != n ? FR_DISK_ERR : fr;
}

const char *settingsio_save(const settings_t *s) {
    UINT got = 0;
    const char *from;
    FRESULT fr = read_text(&got, &from);
    if (fr == FR_NO_FILE || fr == FR_NO_PATH) {
        /* A card without the file gets one: a comment saying what it is,
         * then the lines that differ from the defaults (§11.6). */
        static const char head[] = "# pico-atom: what the machine powers up with\n";
        memcpy(s_text, head, sizeof head - 1u);
        got = sizeof head - 1u;
    } else if (fr != FR_OK) {
        return fr == FR_DENIED ? "FILE TOO BIG" : "CANNOT READ";
    }

    const char *text;
    size_t len;
    settings_status_t st = settings_rewrite(s_text, got, s, &text, &len);
    if (st != SET_OK) {
        printf("  settings     : not saved: %s\n", settings_status_str(st));
        return settings_status_str(st);
    }

    (void)f_mkdir("/atom");
    fr = f_open(&s_file, SETTINGSIO_TEMP, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr == FR_OK) {
        fr = write_all(text, len);
        FRESULT fc = f_close(&s_file);
        if (fr == FR_OK) fr = fc;
    }
    if (fr == FR_OK) {
        (void)f_unlink(SETTINGSIO_PATH);
        fr = f_rename(SETTINGSIO_TEMP, SETTINGSIO_PATH);
    }
    if (fr != FR_OK) {
        printf("  settings     : not saved: FatFs error %d\n", (int)fr);
        return "WRITE FAILED";
    }
    printf("  settings     : %s saved, %u bytes\n", SETTINGSIO_PATH, (unsigned)len);
    return NULL;
}

const char *settingsio_error(void) {
    return s_error;
}
