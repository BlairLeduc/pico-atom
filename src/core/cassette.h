/* cassette.h — tape phase 2: the cassette input at signal level
 * (design.md §11.3).
 *
 * A UEF image plays into port C bit 5 as a square wave clocked in guest
 * cycles, so whatever reads it — the MOS's own byte routine at #FBEE, or
 * a game's loader with its own — sees what it would see from a
 * recorder. Port C bit 4 is the Atom's 2.4 kHz reference, which the
 * MOS's writer times every bit against (#FCD8).
 *
 * Nothing is clocked per instruction. Both bits are brought up to date
 * when port C is read (bus.c), which is the only time they can be
 * observed, so a machine with no tape playing pays nothing for this and
 * one with a tape playing pays per read, not per cycle.
 *
 * The deck has play and stop and no motor control, as the Atom has
 * none: a tape that is playing keeps playing, in guest time, until it
 * is stopped or runs out. Guest time is what moves it, so a paused guest
 * pauses the tape, and a guest run faster than real time — turbo, while
 * a tape plays (main.c) — loads faster by exactly as much.
 *
 * The image is the caller's buffer and must outlive the insertion.
 */
#ifndef PICO_ATOM_CASSETTE_H
#define PICO_ATOM_CASSETTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uef.h"

typedef struct {
    uef_t    uef;
    bool     loaded;
    bool     playing;
    bool     ended;         /* played to the end since the last rewind  */
    bool     level;         /* port C bit 5                              */

    /* The next change on the input, in guest cycles, and whether it
     * changes the level (a half-cycle) or only ends a gap. */
    uint64_t edge;
    bool     edge_toggles;
    uint32_t acc;           /* remainder of units x CPU Hz / (4 x base)  */
    uint64_t paused_left;   /* edge - now, while stopped                 */

    uint32_t hz_ref;        /* a cycle count at which bit 4 went high    */
    /* The first cycle, low 32 bits, at which port C bits 4-5 may next
     * differ from what the last sync left: bit 4's next change, or the
     * tape's next edge if that is sooner. atom.h's inline check. */
    uint32_t ref_next;
    uint32_t edges;         /* level changes played, for the heartbeat   */
} cassette_t;

void cassette_init(cassette_t *c);

/* Load an uncompressed UEF image, rewound and stopped. False, and the
 * deck left empty, if it is not one. */
bool cassette_insert(cassette_t *c, const uint8_t *img, size_t len);
void cassette_eject(cassette_t *c);

void cassette_play(cassette_t *c, uint64_t now, bool on);
void cassette_rewind(cassette_t *c, uint64_t now);

/* The guest clock jumped from `from` to `to` — a snapshot was restored —
 * and the tape carries on from where it is, at the new time. */
void cassette_retime(cassette_t *c, uint64_t from, uint64_t to);

/* Bring the tape up to `now` and return the input level. */
bool cassette_input(cassette_t *c, uint64_t now);

/* The 2.4 kHz reference at `now`: high for the first half of each
 * period. */
bool cassette_ref_2400(cassette_t *c, uint64_t now);

unsigned cassette_percent(const cassette_t *c);

#endif /* PICO_ATOM_CASSETTE_H */
