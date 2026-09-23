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
#define KM_NOCELL (KM_REPT | KM_BREAK | KM_MENU)

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
    uint8_t canon;        /* keymap_picocalc_canonical() of the press */
    uint8_t entry;        /* index into keymap_picocalc, chosen at press */
    uint8_t fields;       /* fields it has been down for */
} keymatrix_held_t;

typedef struct { uint8_t state, code; } keymatrix_event_t;

typedef struct {
    /* Events wait here and are replayed at field rate, in order. */
    keymatrix_event_t queue[ATOM_KEY_EVENT_QUEUE];
    uint8_t q_head, q_len;
    uint8_t gap;          /* fields before the next press may apply */
    uint32_t dropped;     /* events lost to a full queue */

    keymatrix_held_t held[ATOM_KEY_HELD_MAX];
    uint8_t n;
    bool alt, ctrl;

    /* Set on a press, cleared by whoever acts on it. */
    bool menu_request;
} keymatrix_t;

void keymatrix_init(keymatrix_t *k);

/* One [state, code] event off the southbridge FIFO. Queued, not applied:
 * see keymatrix_field. */
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
