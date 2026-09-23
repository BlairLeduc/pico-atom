/* keymatrix.c — the held-key set and the matrix it drives (design.md §10.2). */

#include "keymatrix.h"

#include <string.h>

void keymatrix_init(keymatrix_t *k) {
    memset(k, 0, sizeof(*k));
}

void keymatrix_set_layout(keymatrix_t *k, const keylayout_t *l) {
    k->layout = l;
}

static void enqueue(keymatrix_t *k, uint8_t state, uint8_t code) {
    unsigned tail = (k->q_head + k->q_len) % ATOM_KEY_EVENT_QUEUE;
    k->queue[tail] = (keymatrix_event_t){ state, code };
    k->q_len++;
}

/* Held-state identity is the physical key, not the code (§10.2). */
static int find_open(const keymatrix_t *k, uint8_t canon) {
    for (int i = 0; i < k->n_open; i++) {
        if (k->open[i] == canon) return i;
    }
    return -1;
}

void keymatrix_event(keymatrix_t *k, uint8_t state, uint8_t code) {
    uint8_t canon = keymap_picocalc_canonical(code);
    int o = find_open(k, canon);

    if (state == KEY_EV_RELEASED) {
        /* A release for a press that was refused, or that came before
         * keymatrix_init, has nothing to undo. */
        if (o < 0) return;
        k->open[o] = k->open[--k->n_open];
        enqueue(k, state, code);   /* its slot was kept (below) */
        return;
    }
    if (state != KEY_EV_PRESSED && state != KEY_EV_HELD) return;
    if (o >= 0) return;            /* already down: auto-repeat, or held */

    /* Room for this press, its release, and every release still owed:
     * q_len + n_open never exceeds the queue. */
    if (k->q_len + k->n_open + 2u > ATOM_KEY_EVENT_QUEUE) {
        k->dropped++;
        return;
    }
    k->open[k->n_open++] = canon;
    enqueue(k, state, code);
}

static int find_held(const keymatrix_t *k, uint8_t canon) {
    for (int i = 0; i < k->n; i++) {
        if (k->held[i].canon == canon) return i;
    }
    return -1;
}

/* The entry for a code, chosen once at press time. With Alt down it is
 * the Alt layer and nothing else, so a layout can never take the menu,
 * BREAK or COPY away; an Alt chord with no binding maps to nothing, so
 * the layer never leaks a shifted letter. Otherwise the layout comes
 * first, by physical key, then the plain table (§10.5). */
static const keymap_t *lookup(const keymatrix_t *k, uint8_t code) {
    if (!k->alt && k->layout) {
        uint8_t canon = keymap_picocalc_canonical(code);
        for (unsigned i = 0; i < k->layout->n; i++) {
            if (k->layout->bind[i].code == canon) return &k->layout->bind[i];
        }
    }
    uint8_t want = k->alt ? KM_ALT : 0;
    for (size_t i = 0; i < keymap_picocalc_len; i++) {
        const keymap_t *e = &keymap_picocalc[i];
        if (e->code == code && (e->flags & KM_ALT) == want) return e;
    }
    return NULL;
}

static bool is_modifier(uint8_t code) {
    return code == PICOCALC_KEY_ALT || code == PICOCALC_KEY_CTRL ||
           code == PICOCALC_KEY_SHIFT_L || code == PICOCALC_KEY_SHIFT_R;
}

/* Apply the event at the head of the queue, or say why it must wait. */
static bool apply_head(keymatrix_t *k) {
    keymatrix_event_t ev = k->queue[k->q_head];
    bool down = (ev.state == KEY_EV_PRESSED || ev.state == KEY_EV_HELD);

    if (is_modifier(ev.code)) {
        /* The MCU has already applied Shift to the code, and the keymap
         * says per key whether the Atom's SHIFT goes with it, so the
         * host's Shift is not tracked. Modifiers report held events
         * while down (hardware-notes.md §6.2). */
        if (ev.code == PICOCALC_KEY_ALT)  k->alt  = down;
        if (ev.code == PICOCALC_KEY_CTRL) k->ctrl = down;
        return true;
    }

    uint8_t canon = keymap_picocalc_canonical(ev.code);
    int h = find_held(k, canon);

    if (ev.state == KEY_EV_RELEASED) {
        if (h < 0) return true;                  /* never mapped */
        if (k->held[h].fields < ATOM_KEY_MIN_FIELDS) return false;
        k->held[h] = k->held[--k->n];
        k->gap = ATOM_KEY_GAP_FIELDS;
        return true;
    }
    if (!down) return true;                      /* unknown state */

    /* Auto-repeat arrives as more presses (§6.2). The key is already
     * down; the Atom has its own REPT. */
    if (h >= 0) return true;
    if (k->gap > 0) return false;

    const keymap_t *e = lookup(k, ev.code);
    if (!e) return true;
    if (e->flags & KM_MENU) k->menu_request = true;
    if (k->n >= ATOM_KEY_HELD_MAX) return true;  /* more keys than fingers */

    k->held[k->n++] = (keymatrix_held_t){ .canon = canon, .map = *e };
    return true;
}

void keymatrix_field(keymatrix_t *k, atom_t *m) {
    while (k->q_len > 0 && apply_head(k)) {
        k->q_head = (uint8_t)((k->q_head + 1u) % ATOM_KEY_EVENT_QUEUE);
        k->q_len--;
    }

    bool shift = false, ctrl = k->ctrl, rept = false, brk = false;
    memset(m->key_col, 0, sizeof(m->key_col));
    for (uint8_t i = 0; i < k->n; i++) {
        keymatrix_held_t *h = &k->held[i];
        const keymap_t *e = &h->map;
        if (h->fields < UINT8_MAX) h->fields++;

        /* A layout's CTRL is OR-ed with the host's own (§10.5). */
        if (e->flags & KM_SHIFT) shift = true;
        if (e->flags & KM_CTRL)  ctrl = true;
        if (e->flags & KM_REPT)  rept = true;
        if (e->flags & KM_BREAK) brk = true;
        if (e->flags & KM_NOCELL) continue;
        m->key_col[e->col] |= (uint8_t)(1u << e->row);
    }
    if (k->gap > 0) k->gap--;

    /* BREAK is the 6502's reset line (§6.4): while it is held the machine
     * is reset every field, and the MOS starts once it is let go. */
    if (brk) m->cpu.reset_pending = true;

    atom_key_mods(m, shift, ctrl, rept);   /* refreshes the PPI too */
}
