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
#include "board.h"
#include "keymatrix.h"
#include "roms.h"
#include "settings.h"

typedef struct {
    unsigned volume;      /* 0-8; core 0 applies it as volume * 32      */

    /* Bumped by the menu after it changes `volume`: core 0, parked,
     * applies the volume and plays a short beep at it (main.c). */
    volatile uint32_t beep;

    /* The display page's (design.md §8.7). The menu applies them
     * itself, since the presenter is core 1's too. */
    bool     mono;
    bool     border;
    bool     dark_bg;
    bool     status;      /* the status line (§8.2); core 1 draws it too */
    bool     perf;        /* the perf line (§13.1); core 1 draws it too  */

    /* VIA port B on the GPIOs (§7.4), set by the menu through
     * portb_set as they change. */
    bool     port_b;
    uint8_t  pb_gpio[SETTINGS_PB_BITS];

    /* 1-15 once the settings file or the menu has set it; 0 leaves the
     * southbridge's own level, and a save leaves the file's line. */
    unsigned backlight;

    /* Run unpaced while a UEF plays (§11.3); the settings file's, and
     * not on the menu. */
    bool     turbo;

    /* The game keymap, NULL for the standard map (§10.5), and the ATM
     * name of the tape whose load chose it, "" if the user did. Core 0
     * applies it once it has the machine back. */
    const keylayout_t *layout;
    char     keys_tape[ATOM_ATM_NAME_LEN + 1];

    /* What the settings file said at boot, updated by each save: the
     * keys the menu does not set are saved from here (§11.6). */
    settings_t *file;

    /* The running machine's utility ROM, "" for none (§13.1); the rest
     * of the machine is the atom_t's cfg. */
    char     utility[ROMS_NAME_MAX];

    /* What the About page shows (§13.1): the board from boot, and the
     * ROMs as the last power-on loaded them. */
    const board_info_t  *board;
    const roms_report_t *roms;

    /* The Machine page's Apply and restart: main.c's machine_power_on,
     * with core 0 parked. NULL, or why the machine was left running. */
    const char *(*restart)(const atom_config_t *cfg, const char *utility);
} menu_settings_t;

/* F1-F5 open the main page's first five items from the running Atom or
 * from Pause (§13). Inside the menu they do nothing. */
#define MENU_FKEYS 5

/* Alt+H and F10 open a page of these keys, from the same places. */
#define MENU_PAGE_HELP KM_PAGE_HELP
_Static_assert(MENU_PAGE_HELP > MENU_FKEYS, "the help page is none of the items'");

/* Run the menu until it is closed. `vram` is a page core 1 owns. `page`
 * is 0 for the main page, 1-MENU_FKEYS to open with that item's page, as
 * F1-F5 ask, or MENU_PAGE_HELP; closing that page closes the menu. */
void menu_run(atom_t *m, menu_settings_t *set, uint8_t *vram, unsigned page);

#endif /* PICO_ATOM_MENU_H */
