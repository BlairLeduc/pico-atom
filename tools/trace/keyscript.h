/* keyscript.h — the keys both tracers press, read from one file so the
 * two machines are typed at identically (design.md §15.1).
 *
 * One press per line, pressed at a field and released `hold` fields
 * later. A field here is KS_FIELD_CYCLES guest cycles from the first
 * instruction, on both machines, so both are pressed at the same guest
 * cycle even though their fields are not the same length:
 *
 *   <field> <hold> <column> <bit>     a matrix cell: port A column 0-9,
 *                                     port B bit 0-5
 *   <field> <hold> shift|ctrl|rept    port B bit 7, bit 6, port C bit 6
 *
 * '#' starts a comment. tools/trace-diff.py writes these from text.
 */
#ifndef PICO_ATOM_TRACE_KEYSCRIPT_H
#define PICO_ATOM_TRACE_KEYSCRIPT_H

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { KS_CELL, KS_SHIFT, KS_CTRL, KS_REPT };

typedef struct {
    unsigned field;
    bool     down;
    int      kind;
    unsigned col, bit;
} ks_event_t;

#define KS_MAX_EVENTS 4096
#define KS_FIELD_CYCLES 16667ull     /* 1 MHz / 60 Hz (§12.1) */

typedef struct {
    ks_event_t ev[KS_MAX_EVENTS];
    unsigned   n, next;
} keyscript_t;

static inline int ks_cmp(const void *a, const void *b) {
    const ks_event_t *x = a, *y = b;
    if (x->field != y->field) return x->field < y->field ? -1 : 1;
    /* Releases first, so a key pressed again the field it lets go is a
     * new press. */
    return (int)x->down - (int)y->down;
}

/* False, with a message on stderr, if the file cannot be read. */
static inline bool ks_load(keyscript_t *ks, const char *path) {
    ks->n = ks->next = 0;
    if (!path) return true;
    FILE *f = fopen(path, "r");
    if (!f) { fprintf(stderr, "%s: cannot open\n", path); return false; }
    char line[256];
    unsigned lineno = 0;
    while (fgets(line, sizeof line, f)) {
        lineno++;
        char *hash = strchr(line, '#');
        if (hash) *hash = '\0';
        unsigned field, hold, col, bit;
        char word[16];
        ks_event_t e = { 0 };
        if (sscanf(line, "%u %u %u %u", &field, &hold, &col, &bit) == 4 &&
            col < 10 && bit < 6) {
            e.kind = KS_CELL; e.col = col; e.bit = bit;
        } else if (sscanf(line, "%u %u %15s", &field, &hold, word) == 3) {
            if      (!strcmp(word, "shift")) e.kind = KS_SHIFT;
            else if (!strcmp(word, "ctrl"))  e.kind = KS_CTRL;
            else if (!strcmp(word, "rept"))  e.kind = KS_REPT;
            else { fprintf(stderr, "%s:%u: bad key\n", path, lineno); fclose(f); return false; }
        } else {
            continue;   /* blank or comment */
        }
        if (ks->n + 2 > KS_MAX_EVENTS) { fprintf(stderr, "%s: too many keys\n", path); fclose(f); return false; }
        e.field = field; e.down = true;
        ks->ev[ks->n++] = e;
        e.field = field + (hold ? hold : 1); e.down = false;
        ks->ev[ks->n++] = e;
    }
    fclose(f);
    qsort(ks->ev, ks->n, sizeof ks->ev[0], ks_cmp);
    return true;
}

/* The next event due by `cycles` from the first instruction, or NULL. */
static inline const ks_event_t *ks_due(keyscript_t *ks, unsigned long long cycles) {
    if (ks->next < ks->n && ks->ev[ks->next].field * KS_FIELD_CYCLES <= cycles)
        return &ks->ev[ks->next++];
    return NULL;
}

#endif /* PICO_ATOM_TRACE_KEYSCRIPT_H */
