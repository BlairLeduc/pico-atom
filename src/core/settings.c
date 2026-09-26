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
    s->status    = true;
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

static settings_status_t background(settings_t *s, const char *v) {
    if (same_name(v, "dark")) s->dark_bg = true;
    else if (same_name(v, "black")) s->dark_bg = false;
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
    K_SCREEN, K_BORDER, K_BACKGROUND, K_STATUS, K_BACKLIGHT, K_VOLUME, K_KEYS, K_TAPE,
    K_TURBO, K_DRIVE0, K_DRIVE1, K_UPPER_RAM, K_DOS, K_COUNT
};

static const char *const k_names[K_COUNT] = {
    "screen", "border", "background", "status", "backlight", "volume", "keys", "tape",
    "turbo", "drive0", "drive1", "upper_ram", "dos",
};

static settings_status_t apply(settings_t *s, unsigned k, const char *v) {
    if (!*v && k != K_TAPE && k != K_DRIVE0 && k != K_DRIVE1) return SET_SYNTAX;
    switch (k) {
    case K_SCREEN:    return screen(s, v);
    case K_BORDER:    return on_off(v, &s->border);
    case K_BACKGROUND: return background(s, v);
    case K_STATUS:    return on_off(v, &s->status);
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

/* One line, NUL-terminated and writable. *key is the setting it named,
 * or K_COUNT if it named none. */
static settings_status_t parse_line(settings_t *s, char *line, unsigned *seen, unsigned *key) {
    *key = K_COUNT;
    strip_comment(line);
    char *t = trim(line);
    if (!*t) return SET_OK;

    char *eq = strchr(t, '=');
    if (!eq) return SET_SYNTAX;
    *eq = 0;
    char *name = trim(t), *val = trim(eq + 1);
    if (!*name) return SET_SYNTAX;

    for (unsigned k = 0; k < K_COUNT; k++) {
        if (!same_name(name, k_names[k])) continue;
        *key = k;
        if (*seen & (1u << k)) return SET_DUPLICATE;
        settings_status_t st = apply(s, k, val);
        if (st == SET_OK) *seen |= 1u << k;
        return st;
    }
    return SET_UNKNOWN;
}

/* A line of `n` bytes as the parser sees it: too long, not text, or
 * parsed over *s. */
static settings_status_t classify(settings_t *s, const char *text, size_t n, unsigned *seen,
                                  unsigned *key) {
    char buf[LINE_MAX_LEN + 1];
    *key = K_COUNT;
    if (n > LINE_MAX_LEN) return SET_TOO_LONG;
    memcpy(buf, text, n);
    buf[n] = 0;
    /* A NUL inside the line is not text. */
    if (strlen(buf) != n) return SET_SYNTAX;
    return parse_line(s, buf, seen, key);
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

        unsigned key;
        settings_status_t st = classify(s, text + at, end - at, &seen, &key);
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
    case SET_MISMATCH:  return "would not read back";
    }
    return "?";
}

/* ---- saving the menu's settings (§11.6) ----------------------------------- */

static bool saved_key(unsigned k) {
    switch (k) {
    case K_SCREEN: case K_BORDER: case K_BACKGROUND: case K_STATUS: case K_BACKLIGHT:
    case K_VOLUME: case K_KEYS: case K_TAPE: case K_DRIVE0: case K_DRIVE1:
        return true;
    }
    return false;
}

/* A bare name is in `dir`; FAT's names are the same in either case. */
static bool same_path(const char *dir, const char *a, const char *b) {
    char pa[ATOM_PATH_MAX + 16], pb[ATOM_PATH_MAX + 16];
    const char *x[2] = { a, b };
    char *p[2] = { pa, pb };
    for (unsigned i = 0; i < 2; i++) {
        size_t n = strlen(x[i]);
        if (!n || x[i][0] == '/') {
            memcpy(p[i], x[i], n + 1);
        } else {
            size_t d = strlen(dir);
            memcpy(p[i], dir, d);
            p[i][d] = '/';
            memcpy(p[i] + d + 1, x[i], n + 1);
        }
    }
    return same_name(pa, pb);
}

static bool key_equal(unsigned k, const settings_t *a, const settings_t *b) {
    switch (k) {
    case K_SCREEN:     return a->mono == b->mono;
    case K_BORDER:     return a->border == b->border;
    case K_BACKGROUND: return a->dark_bg == b->dark_bg;
    case K_STATUS:     return a->status == b->status;
    case K_BACKLIGHT:  return a->backlight == b->backlight;
    case K_VOLUME:     return a->volume == b->volume;
    case K_KEYS:       return strcmp(a->keys, b->keys) == 0;
    case K_TAPE:       return same_path(SETTINGS_TAPE_DIR, a->tape, b->tape);
    case K_DRIVE0:     return same_path(SETTINGS_DISC_DIR, a->drive[0], b->drive[0]);
    case K_DRIVE1:     return same_path(SETTINGS_DISC_DIR, a->drive[1], b->drive[1]);
    }
    return true;
}

/* The value a save writes for k. */
static const char *value_of(unsigned k, const settings_t *s, char num[4]) {
    switch (k) {
    case K_SCREEN:     return s->mono ? "mono" : "colour";
    case K_BORDER:     return s->border ? "on" : "off";
    case K_BACKGROUND: return s->dark_bg ? "dark" : "black";
    case K_STATUS:     return s->status ? "on" : "off";
    case K_BACKLIGHT:
    case K_VOLUME: {
        unsigned v = k == K_VOLUME ? s->volume : s->backlight;
        num[0] = (char)('0' + v / 10u % 10u);
        num[1] = (char)('0' + v % 10u);
        num[2] = 0;
        return v < 10u ? num + 1 : num;
    }
    case K_KEYS:       return s->keys[0] ? s->keys : "standard";
    case K_TAPE:       return s->tape;
    case K_DRIVE0:     return s->drive[0];
    case K_DRIVE1:     return s->drive[1];
    }
    return "";
}

static bool written(unsigned k, const settings_t *s) {
    return saved_key(k) && !(k == K_BACKLIGHT && s->backlight == 0);
}

typedef struct {
    char  *buf;
    size_t n;
    bool   over;
} out_t;

static void emit(out_t *o, const char *p, size_t n) {
    if (o->over || n > ATOM_SETTINGS_FILE_MAX - o->n) { o->over = true; return; }
    memcpy(o->buf + o->n, p, n);
    o->n += n;
}

static void emit_str(out_t *o, const char *p) {
    emit(o, p, strlen(p));
}

/* A line for key k whose value no longer says what *s does: the value
 * replaced, and everything around it kept. The value goes where the old
 * one began, or one space after the `=` if the old one was empty, and a
 * comment keeps its column if the new value leaves room, so the file's
 * columns of names, values and comments stay lined up. A gap with a tab
 * in it is kept as it was, since its width is the editor's. */
static void emit_changed(out_t *o, const char *line, size_t n, const char *value) {
    size_t body = n && line[n - 1] == '\r' ? n - 1u : n;
    size_t cend = body;
    for (size_t i = 0; i < body; i++) {
        if (line[i] == '#' && (i == 0 || isspace((unsigned char)line[i - 1]))) { cend = i; break; }
    }
    const char *eq = memchr(line, '=', cend);
    size_t after = eq ? (size_t)(eq - line) + 1u : cend;
    size_t vs = after;
    while (vs < cend && isspace((unsigned char)line[vs])) vs++;
    size_t ve = cend;
    while (ve > vs && isspace((unsigned char)line[ve - 1])) ve--;
    if (vs == ve) vs = after < cend && (line[after] == ' ' || line[after] == '\t') ? after + 1u : after;
    emit(o, line, vs);
    emit_str(o, value);
    if (cend == body) {
        /* No comment: whatever followed the value, and the line's end. */
        emit(o, line + (ve > vs ? ve : vs), n - (ve > vs ? ve : vs));
        return;
    }
    size_t gap_from = ve > vs ? ve : vs;
    if (memchr(line + gap_from, '\t', cend - gap_from)) {
        emit(o, line + gap_from, n - gap_from);
        return;
    }
    size_t col = vs + strlen(value);
    for (size_t pad = cend > col ? cend - col : 1u; pad; pad--) emit(o, " ", 1);
    emit(o, line + cend, n - cend);
}

settings_status_t settings_rewrite(const char *text, size_t len, const settings_t *s,
                                   const char **out, size_t *out_len) {
    static char buf[ATOM_SETTINGS_FILE_MAX];
    out_t o = { buf, 0, false };
    const char *eol = "\n";
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') { if (i && text[i - 1] == '\r') eol = "\r\n"; break; }
    }

    settings_t def;
    settings_default(&def);
    unsigned seen = 0, present = 0;
    size_t at = 0;
    while (at < len) {
        size_t end = at;
        while (end < len && text[end] != '\n') end++;
        size_t n = end - at;

        /* The line as the parser reads it, over the defaults: what it
         * says for its key, if it names one that applies. */
        settings_t line_says = def;
        unsigned key;
        settings_status_t st = classify(&line_says, text + at, n, &seen, &key);
        if (st == SET_DUPLICATE) return SET_DUPLICATE;
        bool applies = st == SET_OK && key < K_COUNT;

        if (applies && written(key, s) && !key_equal(key, &line_says, s)) {
            char num[4];
            emit_changed(&o, text + at, n, value_of(key, s, num));
        } else {
            emit(&o, text + at, n);
        }
        if (applies) present |= 1u << key;
        if (end < len) emit(&o, "\n", 1);
        at = end + 1;
    }

    for (unsigned k = 0; k < K_COUNT; k++) {
        if (!written(k, s) || (present & (1u << k)) || key_equal(k, &def, s)) continue;
        if (o.n && buf[o.n - 1] != '\n') emit_str(&o, eol);
        char num[4];
        emit_str(&o, k_names[k]);
        emit_str(&o, " = ");
        emit_str(&o, value_of(k, s, num));
        emit_str(&o, eol);
    }
    if (o.over) return SET_TOO_LONG;

    /* A rewrite that disagrees with its own reader is a bug, caught here
     * rather than at the next power-on. */
    settings_t back;
    settings_default(&back);
    unsigned line;
    (void)settings_parse(&back, buf, o.n, &line);
    for (unsigned k = 0; k < K_COUNT; k++) {
        if (written(k, s) && !key_equal(k, &back, s)) return SET_MISMATCH;
    }
    *out = buf;
    *out_len = o.n;
    return SET_OK;
}

void settings_card_name(const char *dir, const char *path, char out[ATOM_PATH_MAX]) {
    size_t d = strlen(dir), n = strlen(path);
    const char *v = path;
    if (n > d + 1u && path[d] == '/' && !strchr(path + d + 1, '/')) {
        bool in_dir = true;
        for (size_t i = 0; i < d; i++) {
            if (toupper((unsigned char)path[i]) != toupper((unsigned char)dir[i])) in_dir = false;
        }
        if (in_dir) v = path + d + 1;
    }
    size_t vn = strlen(v);
    if (vn >= ATOM_PATH_MAX) vn = ATOM_PATH_MAX - 1u;
    memcpy(out, v, vn);
    out[vn] = 0;
}
