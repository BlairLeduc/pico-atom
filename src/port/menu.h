/* menu.h — the emulator's menu, opened with Alt+M (design.md §13).
 *
 * Core 1, with the guest parked: the menu is the application boundary
 * at which card work happens (§11.1), so the machine is core 1's for as
 * long as it is open (main.c). It draws a text page through the ordinary
 * renderer in place of the Atom's screen, and closing it is
 * display_invalidate(): the next snapshot repaints everything, so there
 * is nothing to restore (§13).
 *
 * The keyboard is core 1's while the menu is open: it drains the
 * southbridge FIFO and consumes the events itself, since core 0, their
 * usual consumer, is parked.
 */
#ifndef PICO_ATOM_MENU_H
#define PICO_ATOM_MENU_H

#include <stdint.h>

#include "atom.h"

typedef struct {
    unsigned volume;      /* 0-8; core 0 applies it as volume * 32      */
} menu_settings_t;

/* Run the menu until it is closed. `vram` is a page core 1 owns. */
void menu_run(atom_t *m, menu_settings_t *set, uint8_t *vram);

#endif /* PICO_ATOM_MENU_H */
