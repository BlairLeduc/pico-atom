/* settings.c — defaults and the settings file (settings.h, §11.7). */

#include "settings.h"

#include <ctype.h>
#include <string.h>

/* Room for "drive0 = " and the longest path. */
#define LINE_MAX_LEN (ATOM_PATH_MAX + 32u)

void settings_default(settings_t *s) {
    memset(s, 0, sizeof *s);
    atom_config_default(&s->machine);
    s->mono      = true;
    s->border    = true;
    s->volume    = 8u;
    s->backlight = 0u;
    s->turbo     = true;
}

/* strcasecmp is POSIX, not C11. */
static bool same_name(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (toupper((unsigned char)*a) != toupper((unsigned char)*b)) return false;
    }
    return *a == *b;
}

static char *trim(char *s) {
    while (*s && isspace((unsigned char)*s)) s++;
    char *e = s + strlen(s);
    while (e > s && isspace((unsigned char)e[-1])) *--e = 0;
    return s;
}

static settings_status_t on_off(const char *v, bool *out) {
    if (same_name(v, "on"))  { *out = true;  return SET_OK; }
    if (same_name(v, "off")) { *out = false; return SET_OK; }
    return SET_BAD_VALUE;
}

static settings_status_t number(const char *v, unsigned lo, unsigned hi, unsigned *out) {
    unsigned n = 0;
    if (!*v) return SET_BAD_VALUE;
    for (; *v; v++) {
        if (!isdigit((unsigned char)*v) || n > hi) return SET_BAD_VALUE;
        n = n * 10u + (unsigned)(*v - '0');
    }
    if (n < lo || n > hi) return SET_BAD_VALUE;
    *out = n;
    return SET_OK;
}

static settings_status_t path(const char *v, char out[ATOM_PATH_MAX]) {
    size_t n = strlen(v);
    if (n >= ATOM_PATH_MAX) return SET_TOO_LONG;
    memcpy(out, v, n + 1);
    return SET_OK;
}

static settings_status_t screen(settings_t *s, const char *v) {
    if (same_name(v, "colour") || same_name(v, "color")) s->mono = false;
    else if (same_name(v, "mono")) s->mono = true;
    else return SET_BAD_VALUE;
    return SET_OK;
}

static settings_status_t keys(settings_t *s, const char *v) {
    if (!*v) return SET_BAD_VALUE;
    if (same_name(v, "standard")) { s->keys[0] = 0; return SET_OK; }
    size_t n = strlen(v);
    if (n > ATOM_KEYMAP_NAME_LEN) return SET_TOO_LONG;
    for (size_t i = 0; i <= n; i++) s->keys[i] = (char)toupper((unsigned char)v[i]);
    return SET_OK;
}

/* Every setting the file may give, in the order the README lists them.
 * Only the tape and the drives may be left empty, meaning none. */
enum {
    K_SCREEN, K_BORDER, K_BACKLIGHT, K_VOLUME, K_KEYS, K_TAPE, K_TURBO,
    K_DRIVE0, K_DRIVE1, K_UPPER_RAM, K_DOS, K_COUNT
};

static const char *const k_names[K_COUNT] = {
    "screen", "border", "backlight", "volume", "keys", "tape", "turbo",
    "drive0", "drive1", "upper_ram", "dos",
};

static settings_status_t apply(settings_t *s, unsigned k, const char *v) {
    if (!*v && k != K_TAPE && k != K_DRIVE0 && k != K_DRIVE1) return SET_SYNTAX;
    switch (k) {
    case K_SCREEN:    return screen(s, v);
    case K_BORDER:    return on_off(v, &s->border);
    case K_BACKLIGHT: return number(v, 1u, 15u, &s->backlight);
    case K_VOLUME:    return number(v, 0u, 8u, &s->volume);
    case K_KEYS:      return keys(s, v);
    case K_TAPE:      return path(v, s->tape);
    case K_TURBO:     return on_off(v, &s->turbo);
    case K_DRIVE0:    return path(v, s->drive[0]);
    case K_DRIVE1:    return path(v, s->drive[1]);
    case K_UPPER_RAM: return on_off(v, &s->machine.upper_ram);
    case K_DOS:       return on_off(v, &s->machine.atomdos);
    }
    return SET_UNKNOWN;
}

/* A `#` at the start of a line or after a space starts a comment, so a
 * value may be annotated and a path may still hold a `#` (§11.7). */
static void strip_comment(char *line) {
    for (char *p = line; *p; p++) {
        if (*p == '#' && (p == line || isspace((unsigned char)p[-1]))) {
            *p = 0;
            return;
        }
    }
}

static settings_status_t parse_line(settings_t *s, char *line, unsigned *seen) {
    strip_comment(line);
    char *t = trim(line);
    if (!*t) return SET_OK;

    char *eq = strchr(t, '=');
    if (!eq) return SET_SYNTAX;
    *eq = 0;
    char *key = trim(t), *val = trim(eq + 1);
    if (!*key) return SET_SYNTAX;

    for (unsigned k = 0; k < K_COUNT; k++) {
        if (!same_name(key, k_names[k])) continue;
        if (*seen & (1u << k)) return SET_DUPLICATE;
        settings_status_t st = apply(s, k, val);
        if (st == SET_OK) *seen |= 1u << k;
        return st;
    }
    return SET_UNKNOWN;
}

settings_status_t settings_parse(settings_t *s, const char *text, size_t len,
                                 unsigned *line) {
    settings_status_t first = SET_OK;
    unsigned seen = 0, n_line = 0;
    *line = 0;

    size_t at = 0;
    while (at < len) {
        n_line++;
        size_t end = at;
        while (end < len && text[end] != '\n') end++;

        char buf[LINE_MAX_LEN + 1];
        size_t n = end - at;
        settings_status_t st = SET_OK;
        if (n > LINE_MAX_LEN) {
            st = SET_TOO_LONG;
        } else {
            memcpy(buf, text + at, n);
            buf[n] = 0;
            /* A NUL inside the line is not text. */
            st = strlen(buf) != n ? SET_SYNTAX : parse_line(s, buf, &seen);
        }
        if (st != SET_OK && first == SET_OK) {
            first = st;
            *line = n_line;
        }
        at = end + 1;
    }
    return first;
}

const char *settings_status_str(settings_status_t st) {
    switch (st) {
    case SET_OK:        return "ok";
    case SET_SYNTAX:    return "not KEY = VALUE";
    case SET_UNKNOWN:   return "no such setting";
    case SET_BAD_VALUE: return "no such value";
    case SET_DUPLICATE: return "given twice";
    case SET_TOO_LONG:  return "too long";
    }
    return "?";
}
