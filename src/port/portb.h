/* portb.h — VIA port B on the PicoCalc's free GPIOs (design.md §7.4).
 *
 * Port B was the Atom's user port, wired by owners to their own
 * projects. Each of PB0-PB7 may be given a GPIO (settings.h), and the
 * pins behave as the 6522's do: a bit DDRB makes an output is driven high
 * or low, and an input has the pad's pull-up, so that with nothing
 * attached it reads 1. The levels are 3.3 V, not the Atom's 5 V.
 *
 * Core 1 sets the pins, at boot and from the menu, while core 0 waits or
 * is parked. The hook it installs runs on core 0, inside the guest, on a
 * read of ORB and on a write that moves what the chip drives (atom.h),
 * and once a field for T1's PB7 (main.c).
 *
 * In the development build GP4 and GP5 are UART1's, the log and the keys
 * typed over it (hardware-notes.md §2.7); the build that ships has no
 * UART (PICO_ATOM_UART=0). Port B takes a pin from the UART when it is
 * given one, and hands it back when it is not. While port B has the RX
 * pin the UART's receiver is off, so what arrives on the pin is not
 * typed at the guest.
 */
#ifndef PICO_ATOM_PORTB_H
#define PICO_ATOM_PORTB_H

#include <stdbool.h>
#include <stdint.h>

#include "atom.h"
#include "settings.h"

/* Port B on or off, on these pins; the pins held before are let go
 * first, and the hook is installed in *m, or removed. */
void portb_set(atom_t *m, bool on, const uint8_t pins[SETTINGS_PB_BITS]);

/* After atom_init, which clears the hook: install it again, and drive
 * what the new machine's chip drives. */
void portb_attach(atom_t *m);

/* Whether port B holds the UART's TX pin, which the log goes out on,
 * or its RX pin, which the typed keys come in on. */
bool portb_has_uart_tx(void);
bool portb_has_uart_rx(void);

#endif /* PICO_ATOM_PORTB_H */
