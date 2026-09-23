/* tapeio.h — tape phase 1's files: .atm images in /atom/tapes/ on the
 * card (design.md §11.1, §11.2).
 *
 * The core stalls the CPU on an OSLOAD or OSSAVE and leaves the request
 * in atom_t.tape (tape.h); this serves it from the card. Core 1 only,
 * and only while core 0 is parked and the machine is core 1's (main.c):
 * card latency can exceed both the field and the audio deadline (§11.1).
 *
 * A load finds its file by the name in the ATM header, compared byte for
 * byte as the MOS compares names, and failing that by the file's own
 * name without ".atm", ignoring case; an empty name that no file has
 * takes the inserted tape. A name no file answers to is declined, and
 * the MOS asks for the tape as the real machine would. A
 * save writes <name>.atm, or rewrites the file already answering to
 * that name, through a temporary file.
 */
#ifndef PICO_ATOM_TAPEIO_H
#define PICO_ATOM_TAPEIO_H

#include "atom.h"
#include "config.h"
#include "tape.h"

/* True when a load completed, with the file's ATM header name in
 * `loaded`, which is what chooses a game keymap (design.md §10.5). */
bool tapeio_serve(atom_t *m, char loaded[ATOM_ATM_NAME_LEN + 1]);

/* The menu's view (§13). The card must be mounted (storage.h). */
typedef struct {
    char         path[ATOM_PATH_MAX];
    atm_header_t hdr;
} tapeio_entry_t;

/* Up to max .atm files in /atom/tapes/, in directory order. */
unsigned tapeio_list(tapeio_entry_t *out, unsigned max);

/* The file an empty name loads — `*LOAD ""`, `LOAD ""` — when no file
 * is itself named "". The Atom has one cassette deck and this is what
 * is in it. NULL or "" ejects. */
void        tapeio_insert(const char *path);
const char *tapeio_inserted(void);   /* "" when none */

#endif /* PICO_ATOM_TAPEIO_H */
