/* status.h — what the status line says (design.md §8.2).
 *
 * One row of text in the panel's bottom 16 rows, for what the Atom's
 * own screen cannot show: the deck, and each drive whose head is
 * loaded. Core 1 never reads guest state while the 6502 runs (§4.2), so
 * what the line needs travels in the snapshot: this struct, a few bytes,
 * filled by core 0 at the end of the field. The names of the tape and
 * the discs are core 1's already, and it passes them in. The presenter
 * draws the text only when it changes.
 */
#ifndef PICO_ATOM_STATUS_H
#define PICO_ATOM_STATUS_H

#include <stdint.h>

#include "config.h"

struct atom_s;

typedef enum {
    STATUS_DECK_EMPTY = 0,    /* no UEF in the deck: nothing to say       */
    STATUS_DECK_STOP,
    STATUS_DECK_PLAY,
    STATUS_DECK_END,
    STATUS_DECK_REC,
    STATUS_DECK_FULL,         /* recording, and out of room               */
    STATUS_DECK_PROTECTED,    /* the last try to record was refused       */
} status_deck_t;

typedef struct {
    uint8_t  deck;            /* status_deck_t                            */
    uint8_t  percent;         /* through the tape; while recording, how
                                 much of the deck's room it takes         */
    uint8_t  heads;           /* bit d: drive d's head is loaded (§11.4)  */
    uint8_t  turbo10;         /* guest speed in tenths of real time while
                                 it runs unpaced, 0 when paced; the port's */
    uint16_t errors;          /* bytes the recorder could not frame       */
} atom_status_t;

/* The deck, the heads and the recorder's errors, as they are now. The
 * turbo ratio is left 0, for the port to fill. */
void atom_status(const struct atom_s *m, atom_status_t *st);

/* The line's text: ATOM_STATUS_COLS characters, space-padded, and a NUL.
 * `tape` and `drive[d]` are the paths in the deck and the drives, ""
 * for none; the line shows each without its folder or extension. The
 * deck is on the left and the drives on the right, and the tape's name
 * is shortened to fit. Upper case only, as the character ROM has. */
void status_format(const atom_status_t *st, const char *tape,
                   const char *const drive[ATOM_FDC_DRIVES],
                   char out[ATOM_STATUS_COLS + 1]);

#endif /* PICO_ATOM_STATUS_H */
