/* kbd.h — southbridge key events, from core 1 to core 0 (design.md §10.2).
 *
 * Core 1 owns the I2C bus and polls the FIFO; core 0 owns atom_t and
 * feeds the events to keymatrix. Between them is a single-producer,
 * single-consumer ring, so neither core ever waits on the other.
 */
#ifndef PICO_ATOM_KBD_H
#define PICO_ATOM_KBD_H

#include <stdbool.h>
#include <stdint.h>

/* Core 1, from its loop at 30 Hz (§10.2): drain the whole FIFO, up to its
 * 31 entries, into the ring. Returns the events read. */
unsigned kbd_poll(void);

/* Core 0: take one event, if there is one. */
bool kbd_pop(uint8_t *state, uint8_t *code);

/* Events the ring had no room for. The ring holds more than the MCU's
 * FIFO, so this is zero unless core 0 stops draining. */
uint32_t kbd_overflows(void);

#endif /* PICO_ATOM_KBD_H */
