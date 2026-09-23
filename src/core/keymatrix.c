/* keymatrix.c — the held-key set and the matrix it drives (design.md §10.2). */

#include "keymatrix.h"

#include <string.h>

void keymatrix_init(keymatrix_t *k) {
    memset(k, 0, sizeof(*k));
}

void keymatrix_event(keymatrix_t *k, uint8_t state, uint8_t code) {
    if (k->q_len >= ATOM_KEY_EVENT_QUEUE) {
        k->dropped++;
        return;
    }
    unsigned tail = (k->q_head + k->q_len) % ATOM_KEY_EVENT_QUEUE;
    k->queue[tail] = (keymatrix_event_t){ state, code };
    k->q_len++;
}

static int find_held(const keymatrix_t *k, uint8_t canon) {
    for (int i = 0; i < k->n; i++) {
        if (k->held[i].canon == canon) return i;
    }
    return -1;
}

/* The entry for a code, chosen once at press time: the Alt layer while
 * Alt is down, the plain table otherwise. An Alt chord with no binding
 * maps to nothing, so the layer never leaks a shifted letter. */
static int lookup(const keymatrix_t *k, uint8_t code) {
    uint8_t want = k->alt ? KM_ALT : 0;
    for (size_t i = 0; i < keymap_picocalc_len; i++) {
        const keymap_t *e = &keymap_picocalc[i];
        if (e->code == code && (e->flags & KM_ALT) == want) return (int)i;
    }
    return -1;
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

    int e = lookup(k, ev.code);
    if (e < 0) return true;
    if (keymap_picocalc[e].flags & KM_MENU) k->menu_request = true;
    if (k->n >= ATOM_KEY_HELD_MAX) return true;  /* more keys than fingers */

    k->held[k->n++] = (keymatrix_held_t){ .canon = canon, .entry = (uint8_t)e };
    return true;
}

void keymatrix_field(keymatrix_t *k, atom_t *m) {
    while (k->q_len > 0 && apply_head(k)) {
        k->q_head = (uint8_t)((k->q_head + 1u) % ATOM_KEY_EVENT_QUEUE);
        k->q_len--;
    }

    bool shift = false, rept = false, brk = false;
    memset(m->key_col, 0, sizeof(m->key_col));
    for (uint8_t i = 0; i < k->n; i++) {
        keymatrix_held_t *h = &k->held[i];
        const keymap_t *e = &keymap_picocalc[h->entry];
        if (h->fields < UINT8_MAX) h->fields++;

        if (e->flags & KM_REPT)  rept = true;
        if (e->flags & KM_BREAK) brk = true;
        if (e->flags & KM_NOCELL) continue;
        if (e->flags & KM_SHIFT) shift = true;
        m->key_col[e->col] |= (uint8_t)(1u << e->row);
    }
    if (k->gap > 0) k->gap--;

    /* BREAK is the 6502's reset line (§6.4): while it is held the machine
     * is reset every field, and the MOS starts once it is let go. */
    if (brk) m->cpu.reset_pending = true;

    atom_key_mods(m, shift, k->ctrl, rept);   /* refreshes the PPI too */
}
