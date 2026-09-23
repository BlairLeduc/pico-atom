/* keylayout.c — game keymap files and tape matching (keymatrix.h, §10.5). */

#include "keymatrix.h"

#include <ctype.h>
#include <string.h>

/* Longer than any line a .map file has a use for. */
#define LINE_MAX_LEN 80u

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

static bool one_word(const char *s) {
    for (; *s; s++) {
        if (isspace((unsigned char)*s)) return false;
    }
    return true;
}

static keylayout_status_t parse_tapes(keylayout_t *out, char *v) {
    for (char *p = v; *p;) {
        while (*p && (isspace((unsigned char)*p) || *p == ',')) p++;
        if (!*p) break;
        char *w = p;
        while (*p && !isspace((unsigned char)*p) && *p != ',') p++;
        size_t n = (size_t)(p - w);
        if (n > ATOM_ATM_NAME_LEN) return KL_TOO_LONG;
        if (out->n_tapes >= ATOM_KEYMAP_TAPES) return KL_TOO_MANY;
        memcpy(out->tapes[out->n_tapes], w, n);
        out->tapes[out->n_tapes][n] = 0;
        out->n_tapes++;
    }
    return KL_OK;
}

static keylayout_status_t parse_line(keylayout_t *out, char *line) {
    char *s = trim(line);
    if (!*s) return KL_OK;
    /* '#' starts a comment, except where it is the key being bound. */
    if (*s == '#') {
        const char *p = s + 1;
        while (*p == ' ' || *p == '\t') p++;
        if (*p != '=') return KL_OK;
    }

    /* The first '=' after the key, so "= = CTRL" binds the '=' key. */
    char *eq = strchr(s + 1, '=');
    if (!eq) return KL_SYNTAX;
    *eq = 0;
    char *key = trim(s), *val = trim(eq + 1);
    if (!*key || !*val || !one_word(key)) return KL_SYNTAX;

    if (same_name(key, "name")) {
        if (strlen(val) > ATOM_KEYMAP_NAME_LEN) return KL_TOO_LONG;
        size_t i = 0;
        for (; val[i]; i++) out->name[i] = (char)toupper((unsigned char)val[i]);
        out->name[i] = 0;
        return KL_OK;
    }
    if (same_name(key, "tapes")) return parse_tapes(out, val);

    uint8_t code;
    keymap_t t;
    if (!keymap_picocalc_key_named(key, &code)) return KL_BAD_KEY;
    if (!one_word(val) || !keymap_atom_target_named(val, &t)) return KL_BAD_TARGET;
    for (unsigned i = 0; i < out->n; i++) {
        if (out->bind[i].code == code) return KL_DUPLICATE;
    }
    if (out->n >= ATOM_KEYMAP_BINDINGS) return KL_TOO_MANY;
    t.code = code;
    out->bind[out->n++] = t;
    return KL_OK;
}

keylayout_status_t keylayout_parse(keylayout_t *out, const char *name,
                                   const char *text, size_t len, unsigned *line) {
    memset(out, 0, sizeof *out);
    size_t i = 0;
    for (; name && name[i] && i < ATOM_KEYMAP_NAME_LEN; i++) {
        out->name[i] = (char)toupper((unsigned char)name[i]);
    }
    out->name[i] = 0;

    *line = 0;
    size_t at = 0;
    while (at < len) {
        ++*line;
        size_t end = at;
        while (end < len && text[end] != '\n') end++;

        char buf[LINE_MAX_LEN + 1];
        size_t n = end - at;
        if (n > LINE_MAX_LEN) return KL_SYNTAX;
        memcpy(buf, text + at, n);
        buf[n] = 0;
        /* A NUL inside the line is not text. */
        if (strlen(buf) != n) return KL_SYNTAX;

        keylayout_status_t st = parse_line(out, buf);
        if (st != KL_OK) return st;
        at = end + 1;
    }
    *line = 0;
    return KL_OK;
}

const char *keylayout_status_str(keylayout_status_t st) {
    switch (st) {
    case KL_OK:         return "ok";
    case KL_SYNTAX:     return "not KEY = VALUE";
    case KL_BAD_KEY:    return "no such PicoCalc key";
    case KL_BAD_TARGET: return "no such Atom key";
    case KL_DUPLICATE:  return "key bound twice";
    case KL_TOO_MANY:   return "too many entries";
    case KL_TOO_LONG:   return "name too long";
    }
    return "?";
}

bool keylayout_for_tape(const keylayout_t *l, const char *atm_name) {
    if (!atm_name[0]) return false;
    for (unsigned i = 0; i < l->n_tapes; i++) {
        if (same_name(l->tapes[i], atm_name)) return true;
    }
    return false;
}
