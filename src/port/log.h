/* log.h — core 0's UART output, without blocking core 0 (design.md §12.3).
 *
 * UART1 runs at 115200, about 11.5 bytes a millisecond, and a heartbeat
 * is a few hundred bytes: printf from core 0 would stall the field loop
 * for longer than the PCM queue's slack and cause the very underruns the
 * heartbeat reports. So core 0 formats into a ring and core 1, which
 * polls anyway, moves bytes into the UART FIFO as it has room.
 */
#ifndef PICO_ATOM_LOG_H
#define PICO_ATOM_LOG_H

#include <stdbool.h>

/* Core 0. Formats and queues; drops the line whole, and counts it, if
 * the ring is full. Never waits. */
void log_printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* Core 1. Writes what fits in the UART FIFO now; never waits. */
void log_pump(void);

unsigned log_dropped(void);

#endif /* PICO_ATOM_LOG_H */
