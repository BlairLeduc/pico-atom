/* status.c — what the status line says (status.h, design.md §8.2). */

#include "status.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "atom.h"

void atom_status(const atom_t *m, atom_status_t *st) {
    const cassette_t *c = &m->cas;
    memset(st, 0, sizeof *st);

    if (c->loaded) {
        if (c->rec.on) st->deck = c->rec.full ? STATUS_DECK_FULL : STATUS_DECK_REC;
        else if (c->refused == CAS_REC_PROTECTED) st->deck = STATUS_DECK_PROTECTED;
        else if (c->refused == CAS_REC_FULL) st->deck = STATUS_DECK_FULL;
        else if (c->playing) st->deck = STATUS_DECK_PLAY;
        else if (c->needs_1mhz) st->deck = STATUS_DECK_NEEDS_1MHZ;
        else if (c->ended) st->deck = c->rec.full ? STATUS_DECK_FULL : STATUS_DECK_END;
        else st->deck = STATUS_DECK_STOP;
        st->percent = (uint8_t)(c->rec.on ? cassette_room_percent(c) : cassette_percent(c));
        st->errors = (uint16_t)(c->rec.errors > 0xFFFFu ? 0xFFFFu : c->rec.errors);
    }

    /* One LOAD HEAD output, for whichever drive is selected. */
    const i8271_t *f = &m->fdc;
    if (m->cfg.atomdos && f->drive >= 0 && (f->special[I8271_SR_OUTPUT] & I8271_OUT_LOAD))
        st->heads = (uint8_t)(1u << f->drive);
}

/* A path's file name without its folder or extension, upper case, at
 * most `max` characters. */
static void short_name(const char *path, char *out, size_t max) {
    const char *b = strrchr(path, '/');
    b = b ? b + 1 : path;
    const char *dot = strrchr(b, '.');
    size_t n = dot && dot != b ? (size_t)(dot - b) : strlen(b);
    if (n > max) n = max;
    for (size_t i = 0; i < n; i++) out[i] = (char)toupper((unsigned char)b[i]);
    out[n] = 0;
}

/* As much of `s` as fits in the line from *at. */
static void put(char *out, size_t *at, const char *s) {
    for (; *s && *at < ATOM_STATUS_COLS; s++) out[(*at)++] = *s;
}

static const char *deck_word(uint8_t deck) {
    switch (deck) {
    case STATUS_DECK_STOP:      return "STOP";
    case STATUS_DECK_PLAY:      return "PLAY";
    case STATUS_DECK_END:       return "END";
    case STATUS_DECK_REC:       return "REC";
    case STATUS_DECK_FULL:      return "FULL";
    case STATUS_DECK_PROTECTED: return "PROTECTED";
    case STATUS_DECK_NEEDS_1MHZ: return "NEEDS 1 MHZ";
    }
    return "";
}

void status_format(const atom_status_t *st, const char *tape,
                   const char *const drive[ATOM_FDC_DRIVES],
                   char out[ATOM_STATUS_COLS + 1]) {
    enum { W = ATOM_STATUS_COLS };
    char right[W + 1] = "";
    size_t rn = 0;
    for (unsigned d = 0; d < ATOM_FDC_DRIVES; d++) {
        if (!(st->heads & (1u << d))) continue;
        char name[11];
        short_name(drive[d] ? drive[d] : "", name, 10);
        int k = snprintf(right + rn, sizeof right - rn, "%sD%u %s", rn ? " " : "", d,
                         name[0] ? name : "-");
        if (k > 0) rn += (size_t)k < sizeof right - rn ? (size_t)k : sizeof right - rn - 1u;
    }

    /* The deck: the word, how far, the name, then turbo and errors,
     * with the name given whatever room is left. */
    char head[16] = "", tail[24] = "";
    if (st->deck != STATUS_DECK_EMPTY) {
        bool pct = st->deck == STATUS_DECK_STOP || st->deck == STATUS_DECK_PLAY ||
                   st->deck == STATUS_DECK_REC;
        snprintf(head, sizeof head, pct ? "%s %u%%" : "%s", deck_word(st->deck),
                 (unsigned)st->percent);
        size_t tn = 0;
        if (st->turbo10) {
            tn += (size_t)snprintf(tail + tn, sizeof tail - tn, " %u.%uX",
                                   st->turbo10 / 10u, st->turbo10 % 10u);
        }
        if (st->errors) {
            snprintf(tail + tn, sizeof tail - tn, " %u BAD", (unsigned)st->errors);
        }
    }

    memset(out, ' ', W);
    out[W] = 0;
    if (head[0]) {
        size_t fixed = strlen(head) + strlen(tail) + (rn ? rn + 1u : 0u);
        size_t room = fixed + 1u < W ? W - fixed - 1u : 0u;
        char name[W + 1];
        short_name(tape ? tape : "", name, room);
        size_t at = 0;
        put(out, &at, head);
        if (name[0]) put(out, &at, " ");
        put(out, &at, name);
        put(out, &at, tail);
    }
    if (rn && rn <= W) memcpy(out + W - rn, right, rn);
}

/* ---- the perf line and PAUSED (§13.1) ---------------------------------- */

static unsigned clamp(uint32_t v, unsigned max) {
    return v > max ? max : (unsigned)v;
}

void status_perf_format(const perf_line_t *p, char out[ATOM_STATUS_COLS + 1]) {
    enum { W = ATOM_STATUS_COLS };
    unsigned pct = clamp((p->busy1000 + 5u) / 10u, 100u);
    unsigned head = clamp(p->head100, 999u * 100u + 99u);
    unsigned ms10 = clamp((p->present_us + 50u) / 100u, 9999u);
    /* Two spaces between the groups while they fit, one when the counts
     * have grown, so the last figure is never the one that goes. */
    char text[64];
    int n = 0;
    for (unsigned gap = 2; gap >= 1; gap--) {
        const char *sp = gap == 2 ? "  " : " ";
        n = snprintf(text, sizeof text, "C0 %u%% %u.%02uX%sLCD %u.%uMS%sDROP %u%sUR %u %u",
                     pct, head / 100u, head % 100u, sp, ms10 / 10u, ms10 % 10u, sp,
                     clamp(p->dropped, 999u), sp, clamp(p->underruns, 99999u),
                     clamp(p->late, 9999u));
        if (n >= 0 && (size_t)n <= W) break;
    }
    size_t len = n < 0 ? 0u : (size_t)n < W ? (size_t)n : W;
    memset(out, ' ', W);
    memcpy(out, text, len);
    out[W] = 0;
}

void status_paused_format(char out[ATOM_STATUS_COLS + 1]) {
    enum { W = ATOM_STATUS_COLS };
    static const char text[] = "PAUSED: ANY KEY RESUMES";
    memset(out, ' ', W);
    memcpy(out, text, sizeof text - 1u);
    out[W] = 0;
}
