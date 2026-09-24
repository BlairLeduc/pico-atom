/* settings.h — the emulator's settings at power-on, and the card file
 * that changes them, /atom/pico-atom.cfg (design.md §11.7).
 *
 * settings_default() is where every default lives: the machine's own
 * (atom_config_default) and the host's — the screen, the sound, the keys,
 * and what is in the deck and the drives at boot. The file names only
 * what it changes, one `key = value` a line, as a .map file does (§10.5).
 *
 * The parser is here, in the core, so it is tested on the host. What a
 * value names on the card — a layout, a tape, a disc — is the port's to
 * find, and it says so when it cannot.
 */
#ifndef PICO_ATOM_SETTINGS_H
#define PICO_ATOM_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#include "atom.h"
#include "config.h"

#define SETTINGS_DRIVES 2u

typedef struct {
    /* The guest (§7.2): RAM at #4000-#7FFF, AtomDOS. */
    atom_config_t machine;

    bool     mono;          /* the Display page's screen (§8.7)          */
    bool     border;        /* and its border                            */
    unsigned volume;        /* 0-8, as the menu shows it                 */
    unsigned backlight;     /* 1-15, as the menu shows it; 0 leaves the
                               southbridge's own level alone             */
    bool     turbo;         /* run unpaced while a UEF plays (§11.3)     */

    /* A layout's name, uppercase; "" is the standard map (§10.5). */
    char     keys[ATOM_KEYMAP_NAME_LEN + 1];

    /* As written in the file, "" for none. The port resolves a bare
     * name against /atom/tapes/ or /atom/discs/. */
    char     tape[ATOM_PATH_MAX];
    char     drive[SETTINGS_DRIVES][ATOM_PATH_MAX];
} settings_t;

void settings_default(settings_t *s);

typedef enum {
    SET_OK = 0,
    SET_SYNTAX,         /* not key = value                  */
    SET_UNKNOWN,        /* no such setting                  */
    SET_BAD_VALUE,      /* not one of the setting's values  */
    SET_DUPLICATE,      /* the setting was given twice      */
    SET_TOO_LONG,       /* a line, a name or a path         */
} settings_status_t;

/* Apply the file's text over *s, which holds the defaults or an earlier
 * file's values. A line that is wrong changes nothing and the lines after
 * it still apply, so one typing mistake does not cost the rest of the
 * file. Returns the first line's status, and its number in *line (0 when
 * every line was good). */
settings_status_t settings_parse(settings_t *s, const char *text, size_t len,
                                 unsigned *line);
const char *settings_status_str(settings_status_t st);

#endif /* PICO_ATOM_SETTINGS_H */
