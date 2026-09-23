/* guest.h — a real Atom, on the host, for the tests that need the MOS
 * (design.md §15.1).
 *
 * Needs the user's ROM images, which the tree does not ship (§11.1). It
 * looks in PICO_ATOM_ROMS if that is set, else the repository's roms/
 * staging directory, and recognises images by SHA-1 whatever they are
 * called (romset.h). Keys arrive as southbridge events through
 * keymatrix, paced the way the device paces them (§10.2).
 */
#ifndef PICO_ATOM_TEST_GUEST_H
#define PICO_ATOM_TEST_GUEST_H

#include <stdbool.h>
#include <stdint.h>

#include "atom.h"
#include "keymatrix.h"
#include "romset.h"

typedef struct {
    atom_t      m;
    keymatrix_t k;
    /* Called after every field, as the port drains audio after every
     * field (§12.2). Optional. */
    void (*on_field)(atom_t *m);
} guest_t;

/* Scan for images. True if the kernel and BASIC were both found; *dir
 * is where it looked, for the skip message. */
bool guest_find_roms(const char **dir);
bool guest_have_rom(rom_slot_t s);

/* Build the machine, install every image found, reset, and run two
 * seconds to the prompt. AtomDOS is in, and dormant until *DOS. */
void guest_boot(guest_t *g);

void guest_fields(guest_t *g, int n);

/* Run until every queued event has been replayed and every key is up,
 * then long enough for the MOS to act on the last one. */
void guest_settle(guest_t *g);

/* A key as the MCU sends it: press and release in one poll, the worst
 * case, then the rest of the 30 Hz poll interval before the next. */
void guest_tap(guest_t *g, uint8_t code);

/* With a host modifier held around it, then settle. */
void guest_chord(guest_t *g, uint8_t mod, uint8_t code);

/* Type a string as a PicoCalc user would, then settle. '\n' is RETURN. */
void guest_type(guest_t *g, const char *s);

/* The screen byte the MOS writes for an ASCII character. */
uint8_t guest_screen_code(char c);

/* The text on one alpha row, as ASCII, with inverse and graphics as '#'
 * and trailing spaces trimmed. The returned buffer is reused. */
const char *guest_row(const atom_t *m, int row);

/* The one cell with bit 7 set, or -1 if there is not exactly one. */
int guest_cursor(const atom_t *m);

#endif /* PICO_ATOM_TEST_GUEST_H */
