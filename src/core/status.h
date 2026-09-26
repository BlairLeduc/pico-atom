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
    STATUS_DECK_NEEDS_1MHZ,   /* play refused: the MOS reads at 1 MHz only */
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

/* ---- the perf line (§13.1) ---------------------------------------------- *
 * Host counters, not guest state, so not in atom_status_t: core 0 writes
 * these once a second as whole 32-bit words, each single-copy atomic,
 * and core 1 reads them. A read across a write can mix two seconds,
 * which shows for a second and is harmless. */
typedef struct {
    uint32_t busy1000;      /* core 0's share in the guest, thousandths,
                               over the last second (§6.3)               */
    uint32_t head100;       /* guest cycles per microsecond of it, in
                               hundredths: times real time unpaced       */
    uint32_t present_us;    /* the longest present in the second (§8.4) */
    uint32_t dropped;       /* snapshots dropped in the second          */
    uint32_t underruns;     /* underrun samples since boot (§9.4)       */
    uint32_t late;          /* late refills since boot                  */
} perf_line_t;

/* The perf line's text, ATOM_STATUS_COLS characters space-padded:
 * `C0 43% 2.31X  LCD 11.5MS  DROP 0  UR 0 0`. A figure too wide is
 * shown at its widest rather than pushing the rest off. */
void status_perf_format(const perf_line_t *p, char out[ATOM_STATUS_COLS + 1]);

/* The status line while the guest is paused (§13.1), whether or not
 * the line is on. */
void status_paused_format(char out[ATOM_STATUS_COLS + 1]);

#endif /* PICO_ATOM_STATUS_H */
