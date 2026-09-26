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
 *
 * The recorder is the other half (§11.3): port C bit 0, or the 2.4 kHz
 * reference while bit 1 gates it on, read back as half-cycles and
 * decoded as the reader at #FBEE would, into UEF chunks appended to the
 * image in the deck. It too is brought up to date only when something
 * changes the line — a write to port C (bus.c) — and when it stops, so a
 * 1 bit's sixteen short half-cycles cost no more than the write that
 * starts them. A tape inserted without room to write to is protected.
 */
#ifndef PICO_ATOM_CASSETTE_H
#define PICO_ATOM_CASSETTE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "uef.h"

/* Why the recorder would not start. */
typedef enum {
    CAS_REC_OK = 0,
    CAS_REC_NO_TAPE,      /* no UEF in the deck                        */
    CAS_REC_PROTECTED,    /* inserted read-only: the tab broken off    */
    CAS_REC_FULL,         /* no room left in the deck's buffer         */
} cas_rec_status_t;

typedef struct {
    bool     on;
    bool     full;          /* ran out of room, and records nothing more */
    uint8_t  out;           /* port C bits 0-1, the line's source        */
    bool     level;         /* the line just before `at`                 */
    uint64_t at;            /* the line is decoded up to here            */
    uint64_t last_edge;

    /* The frame being read: bits so far, and the half-cycles of the bit
     * in progress, all of one kind (§11.3). */
    bool     framing;
    uint8_t  bit, halves, kind;
    uint16_t frame;

    uint32_t tone;          /* short half-cycles not yet written          */
    uint32_t gap;           /* silence not yet written, in 1/2400 s       */
    uint32_t data_at;       /* the open &0100 chunk's offset, 0 if none   */
    uint32_t bytes;         /* bytes recorded since it started            */
    uint32_t errors;        /* bytes that did not frame, dropped          */
} cassette_rec_t;

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

    /* Writable: the image may grow to `cap`. NULL for a protected tape. */
    uint8_t *wbuf;
    uint32_t cap;
    bool     dirty;         /* recorded onto since the card last had it  */
    cas_rec_status_t refused;   /* the last start that failed, until the
                                   next start or insertion               */
    cassette_rec_t rec;
} cassette_t;

void cassette_init(cassette_t *c);

/* Load an uncompressed UEF image, rewound and stopped. False, and the
 * deck left empty, if it is not one. */
bool cassette_insert(cassette_t *c, const uint8_t *img, size_t len);
void cassette_eject(cassette_t *c);

/* The same, writable: a recording is appended to the image in `img`,
 * which may grow to `cap` bytes. */
bool cassette_insert_rw(cassette_t *c, uint8_t *img, size_t len, size_t cap);

/* Both do nothing while recording: the recorder has the tape. */
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

/* Start or stop the recorder, with port C's output latch as it is now.
 * Starting stops the tape playing; stopping writes what is still
 * pending and leaves the tape at its end, as recording forward does. */
cas_rec_status_t cassette_record(cassette_t *c, uint64_t now, bool on, uint8_t out_c);

/* Port C's output latch is `out_c` from `now`: bring the recorder up to
 * date with the line as it was, then follow the new one. Only while
 * recording. */
void cassette_output(cassette_t *c, uint64_t now, uint8_t out_c);

/* How much of the deck's room the image takes, 0-100. */
unsigned cassette_room_percent(const cassette_t *c);

#endif /* PICO_ATOM_CASSETTE_H */
