/* keymapio.h — the game keymaps the menu offers: the built-in layouts,
 * then the .map files in /atom/keymaps/ on the card (design.md §10.5,
 * §11.1).
 *
 * Core 1 only. The card's layouts are read at boot and again each time
 * the menu opens, with the card mounted (storage.h) and core 0 parked or
 * not yet started, so the layout core 0 holds a pointer to is never
 * rewritten under it. A file that does not parse is left out, and the
 * first such file and line is kept for the menu's status row.
 */
#ifndef PICO_ATOM_KEYMAPIO_H
#define PICO_ATOM_KEYMAPIO_H

#include "keymatrix.h"

void keymapio_scan(void);

/* Built-in layouts first, then the card's, in directory order. */
unsigned           keymapio_count(void);
const keylayout_t *keymapio_get(unsigned i);

/* The index of the layout with this name, or -1. */
int keymapio_find(const char *name);

/* The first layout whose tapes line names this ATM header, or NULL. */
const keylayout_t *keymapio_for_tape(const char *atm_name);

/* "" when every file parsed; else, e.g., "FIRE.MAP 3: NO SUCH ATOM KEY". */
const char *keymapio_error(void);

#endif /* PICO_ATOM_KEYMAPIO_H */
