/* keymatrix.h — host key events to the Atom's key matrix (design.md §10).
 *
 * The PicoCalc delivers translated characters as press/release events
 * (hardware-notes.md §6.2); the Atom wants a 10x6 matrix plus SHIFT,
 * CTRL and REPT lines. This builds a held-key set from the events and
 * drives the matrix from that set once per field (§10.2), never from a
 * character stream.
 *
 * Runs on core 0, which owns atom_t. Events reach it through the port's
 * queue from core 1, which owns the I2C bus (§4).
 */
#ifndef PICO_ATOM_KEYMATRIX_H
#define PICO_ATOM_KEYMATRIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "atom.h"
#include "config.h"

/* ---- the keymap: data (§10.3) ---------------------------------------- */

#define KM_SHIFT  0x01u   /* assert the Atom SHIFT line with this cell  */
#define KM_ALT    0x02u   /* the entry is on the Alt layer              */
#define KM_REPT   0x04u   /* no cell: the REPT line, port C bit 6       */
#define KM_BREAK  0x08u   /* no cell: BREAK, which is the reset line    */
#define KM_MENU   0x10u   /* no cell: the emulator menu (§13)           */
#define KM_CTRL   0x20u   /* assert the Atom CTRL line with this entry  */
#define KM_LINE   0x40u   /* no cell: KM_SHIFT or KM_CTRL alone (§10.5) */
#define KM_NOCELL (KM_REPT | KM_BREAK | KM_MENU | KM_LINE)

typedef struct {
    uint8_t code;         /* host key code, as translated by the MCU */
    uint8_t row, col;     /* Atom matrix cell; ignored under KM_NOCELL */
    uint8_t flags;
} keymap_t;

extern const keymap_t keymap_picocalc[];
extern const size_t   keymap_picocalc_len;

/* The physical key behind a translated code. The MCU retranslates at
 * every transition, so releasing Shift first turns press 'A' into
 * release 'a' and press '!' into release '1' (hardware-notes.md §6.2);
 * held-state identity is this, not the code. */
uint8_t keymap_picocalc_canonical(uint8_t code);

/* ---- game keymaps: overlays on the standard map (§10.5) -------------- */

/* A binding's code is canonical (keymap_picocalc_canonical), so it holds
 * whichever way the MCU translated the key; its target is a cell with no
 * SHIFT, or a line alone: KM_CTRL | KM_LINE, KM_SHIFT | KM_LINE, KM_REPT. */
typedef struct {
    char     name[ATOM_KEYMAP_NAME_LEN + 1];
    uint8_t  n;
    keymap_t bind[ATOM_KEYMAP_BINDINGS];
    /* ATM header names whose load selects this layout (§11.2). */
    uint8_t  n_tapes;
    char     tapes[ATOM_KEYMAP_TAPES][ATOM_ATM_NAME_LEN + 1];
} keylayout_t;

extern const keylayout_t keylayout_builtin[];
extern const size_t      keylayout_builtin_len;

typedef enum {
    KL_OK = 0,
    KL_SYNTAX,          /* not "word = value", or an empty value       */
    KL_BAD_KEY,         /* no PicoCalc key by that name                */
    KL_BAD_TARGET,      /* no Atom key or line by that name            */
    KL_DUPLICATE,       /* a key bound twice                           */
    KL_TOO_MANY,        /* more bindings or tapes than config.h allows */
    KL_TOO_LONG,        /* a name longer than the menu shows           */
} keylayout_status_t;

/* A .map file's text (§10.5): "name = ...", "tapes = ...", and one
 * "<PicoCalc key> = <Atom target>" per line; '#' starts a comment line.
 * `name` is used when the file gives none. On failure *line is the
 * 1-based line at fault, and `out` must not be used. */
keylayout_status_t keylayout_parse(keylayout_t *out, const char *name,
                                   const char *text, size_t len, unsigned *line);
const char *keylayout_status_str(keylayout_status_t st);

/* Does loading a file with this ATM header name select the layout? Names
 * compare ignoring case: the file is typed by hand. */
bool keylayout_for_tape(const keylayout_t *l, const char *atm_name);

/* The names the parser takes, which are the keymap's to know. Both
 * compare without regard to case. */
bool keymap_picocalc_key_named(const char *name, uint8_t *code);
bool keymap_atom_target_named(const char *name, keymap_t *out);

/* Modifier codes and event states (hardware-notes.md §6.2). */
#define PICOCALC_KEY_ALT      0xA1u
#define PICOCALC_KEY_SHIFT_L  0xA2u
#define PICOCALC_KEY_SHIFT_R  0xA3u
#define PICOCALC_KEY_CTRL     0xA5u

#define KEY_EV_PRESSED   1u
#define KEY_EV_HELD      2u
#define KEY_EV_RELEASED  3u

/* ---- the held-key set ------------------------------------------------ */

typedef struct {
    uint8_t  canon;       /* keymap_picocalc_canonical() of the press */
    uint8_t  fields;      /* fields it has been down for */
    /* The binding chosen at press, copied: its release undoes exactly
     * that, whatever layout is in force by then (§10.5). */
    keymap_t map;
    /* Pressed under the host's Shift for a character the Atom types
     * unshifted, like ':': while it is down the host's Shift does not
     * reach the SHIFT line (§10.3). */
    bool unshift;
} keymatrix_held_t;

typedef struct { uint8_t state, code; } keymatrix_event_t;

typedef struct {
    /* Events wait here and are replayed at field rate, in order. */
    keymatrix_event_t queue[ATOM_KEY_EVENT_QUEUE];
    uint8_t q_head, q_len;
    uint8_t gap;          /* fields before the next press may apply */
    uint32_t dropped;     /* presses refused for want of room */

    /* Keys whose press is queued or applied and whose release has not
     * arrived yet. The queue always keeps a slot for each of their
     * releases: a lost press drops a character, a lost release holds a
     * key down for ever. */
    uint8_t open[ATOM_KEY_EVENT_QUEUE];
    uint8_t n_open;

    keymatrix_held_t held[ATOM_KEY_HELD_MAX];
    uint8_t n;
    bool alt, ctrl;
    uint8_t shift;        /* the host's Shifts that are down: bit 0 left, 1 right */

    /* The game keymap over the standard map, or NULL (§10.5). */
    const keylayout_t *layout;

    /* Set on a press, cleared by whoever acts on it. */
    bool menu_request;
} keymatrix_t;

/* Empty, with no layout. */
void keymatrix_init(keymatrix_t *k);

/* Takes effect from the next press; a key already down keeps the binding
 * it went down with. NULL is the standard map. */
void keymatrix_set_layout(keymatrix_t *k, const keylayout_t *l);

/* One [state, code] event off the southbridge FIFO. Queued, not applied:
 * see keymatrix_field. A press that finds no room for itself and for
 * every outstanding release is refused, and its release with it; a
 * press of a key already down (the MCU's auto-repeat) is absorbed. */
void keymatrix_event(keymatrix_t *k, uint8_t state, uint8_t code);

/* Once per field, before the guest runs: replay queued events, drive the
 * matrix from the held set, and age it.
 *
 * Replay is paced for the MOS, not for the poll. A press and its release
 * can arrive in the same 30 Hz poll (§10.2), and OSRDCH (#FE94) waits
 * for every key to be up before it accepts the next one, so a release
 * waits until its key has been down ATOM_KEY_MIN_FIELDS, and a press
 * waits ATOM_KEY_GAP_FIELDS after a release. Keys that overlapped at
 * the keyboard still overlap here, and lose characters as they would
 * on an Atom; keys that did not, do not. */
void keymatrix_field(keymatrix_t *k, atom_t *m);

#endif /* PICO_ATOM_KEYMATRIX_H */
