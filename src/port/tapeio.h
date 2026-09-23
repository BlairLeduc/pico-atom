/* tapeio.h — the tape files in /atom/tapes/ on the card: .atm images
 * served by phase 1's trap, and .uef images played by phase 2's
 * cassette (design.md §11.1-§11.3).
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

/* A .uef in the deck is decompressed whole into a static buffer
 * (ATOM_UEF_MAX, config.h) and played from there by core 0; the trap
 * stands aside while it is in, so every load reads the signal. Saves
 * still go to .atm files: recording at signal level is not modelled.
 *
 * True when a load completed, with the file's ATM header name in
 * `loaded`, which is what chooses a game keymap (design.md §10.5). */
bool tapeio_serve(atom_t *m, char loaded[ATOM_ATM_NAME_LEN + 1]);

/* The menu's view (§13). The card must be mounted (storage.h). */
typedef struct {
    char         path[ATOM_PATH_MAX];
    bool         uef;
    atm_header_t hdr;       /* .uef: the file name in hdr.name, no more */
    uint32_t     size;      /* bytes on the card                         */
} tapeio_entry_t;

/* Up to max .atm and .uef files in /atom/tapes/, in directory order. */
unsigned tapeio_list(tapeio_entry_t *out, unsigned max);

/* Put a tape in the Atom's one deck. An .atm is what an empty name
 * loads — `*LOAD ""`, `LOAD ""` — when no file is itself named "". A
 * .uef is decompressed into the deck, stopped: the MOS's PLAY TAPE and
 * the key that answers it start it (tape.h's cues, §11.3). NULL or ""
 * ejects. The card must be mounted and core 0 parked. Returns NULL, or
 * why the tape did not go in, and the deck is then empty. */
const char *tapeio_insert(atom_t *m, const char *path);
const char *tapeio_inserted(void);   /* "" when none */

/* The first file on the UEF in the deck, which is what to LOAD by name:
 * an empty name is the ROM's nameless format, not "the next file". */
const char *tapeio_first_name(void);

#endif /* PICO_ATOM_TAPEIO_H */
