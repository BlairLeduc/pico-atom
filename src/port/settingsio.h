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
 *
 * The menu's Save settings writes the file (§11.6): the user's text
 * edited by settings_rewrite, through a temporary file and a rename, so
 * that a pulled card leaves the old file or the new one. A load that
 * finds only the temporary file, a save cut off between the two, takes
 * it.
 */
#ifndef PICO_ATOM_SETTINGSIO_H
#define PICO_ATOM_SETTINGSIO_H

#include "settings.h"

#define SETTINGSIO_PATH "/atom/pico-atom.cfg"
#define SETTINGSIO_TEMP "/atom/pico-atom.new"

/* The file's settings over the defaults, into *out. */
void settingsio_load(settings_t *out);

/* The same, saying nothing and keeping no problem: for main()'s read of
 * the host clock, before stdio is up (design.md §3.2). */
void settingsio_peek(settings_t *out);

/* Something the file names could not be used: logged, and kept for the
 * status row if it is the first problem. */
void settingsio_fail(const char *what, const char *why);

/* "" when there was no problem; else, e.g., "CFG 3: NO SUCH SETTING". */
const char *settingsio_error(void);

/* Write *s into the file, or a new one (settings.h's settings_rewrite).
 * The card must be mounted. NULL, or why the file was left as it was. */
const char *settingsio_save(const settings_t *s);

#endif /* PICO_ATOM_SETTINGSIO_H */
