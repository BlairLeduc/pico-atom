/* fattime.h — a file's timestamp, as FatFs packs it (design.md §11.1).
 *
 * The PicoCalc shell keeps the time in the RP2350's always-on timer; the
 * port reads it and hands the local time here. Kept in the core, without
 * the SDK, so the calendar is checked on the host.
 */
#ifndef PICO_ATOM_FATTIME_H
#define PICO_ATOM_FATTIME_H

#include <stdint.h>

/* FAT's packing: year - 1980 in bits 31-25, month 24-21, day 20-16,
 * hour 15-11, minute 10-5, seconds / 2 in 4-0. */
#define FATTIME_PACK(y, mo, d, h, mi, s)                                         \
    ((uint32_t)((y) - 1980) << 25 | (uint32_t)(mo) << 21 | (uint32_t)(d) << 16 | \
     (uint32_t)(h) << 11 | (uint32_t)(mi) << 5 | (uint32_t)(s) / 2u)

/* The stamp when the shell has not set the clock: the date FatFs used
 * with FF_FS_NORTC, midnight on 2026-01-01. */
#define FATTIME_UNSET FATTIME_PACK(2026, 1, 1, 0, 0, 0)

/* Seconds since 1970-01-01 00:00, local time, packed. FAT holds
 * 1980-2107; a time outside that is clamped to its nearer end. */
uint32_t fattime_pack(int64_t local_s);

#endif
