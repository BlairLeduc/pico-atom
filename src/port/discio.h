/* discio.h — disc images in /atom/discs/ on the card, behind the 8271
 * (design.md §11.1, §11.4).
 *
 * The FDC in the core knows each drive's geometry and asks for bytes: up
 * to a track of sectors to read, or a track's worth it has collected to
 * write (i8271.h). This serves the request from the image file. Core 1
 * only, and only while core 0 is parked and the machine is core 1's
 * (main.c): card latency can exceed both the field and the audio
 * deadline (§11.1).
 *
 * Images are Acorn's: 256-byte sectors, ten to a track. An .ssd, .dsk
 * or .40t is one side, track after track; a .dsd is two, interleaved a
 * track of side 0 then a track of side 1. A short image — an .ssd is
 * often cut off after its last used sector — reads as zeros past its
 * end and grows when written there. A file with the read-only attribute
 * is a write-protected disc.
 */
#ifndef PICO_ATOM_DISCIO_H
#define PICO_ATOM_DISCIO_H

#include <stdbool.h>
#include <stdint.h>

#include "atom.h"
#include "config.h"

/* The request atom_disc_request names; false if the card failed it, in
 * which case the FDC reports a CRC error or a write fault. */
bool discio_serve(atom_t *m);

typedef struct {
    char     path[ATOM_PATH_MAX];
    char     name[24];      /* the file's name, as the menu shows it */
    uint32_t size;
    bool     protect;
} discio_entry_t;

/* Up to max images in /atom/discs/, in directory order. The card must
 * be mounted (storage.h). */
unsigned discio_list(discio_entry_t *out, unsigned max);

/* Put an image in drive 0 or 1; NULL or "" empties the drive. The card
 * must be mounted and core 0 parked. Returns NULL, or why the disc did
 * not go in, and the drive is then empty. */
const char *discio_insert(atom_t *m, unsigned drive, const char *path);
const char *discio_inserted(unsigned drive);   /* "" when none */

#endif /* PICO_ATOM_DISCIO_H */
