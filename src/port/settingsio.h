/* settingsio.h — the settings file on the card, /atom/pico-atom.cfg
 * (design.md §11.1, §11.7).
 *
 * Core 1, at boot, with the card mounted (storage.h) and core 0 waiting:
 * the file is read once, before the ROMs, because some of it is the
 * machine's configuration. There is no file on a new card, and that is
 * not a problem: everything keeps its default (settings.h).
 *
 * The first problem is kept for the menu's status row: a line the parser
 * refused, or something a line named that the card does not have.
 */
#ifndef PICO_ATOM_SETTINGSIO_H
#define PICO_ATOM_SETTINGSIO_H

#include "settings.h"

#define SETTINGSIO_PATH "/atom/pico-atom.cfg"

/* The file's settings over the defaults, into *out. */
void settingsio_load(settings_t *out);

/* Something the file names could not be used: logged, and kept for the
 * status row if it is the first problem. */
void settingsio_fail(const char *what, const char *why);

/* "" when there was no problem; else, e.g., "CFG 3: NO SUCH SETTING". */
const char *settingsio_error(void);

#endif /* PICO_ATOM_SETTINGSIO_H */
