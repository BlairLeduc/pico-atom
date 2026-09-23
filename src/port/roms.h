/* roms.h — load the ROM set off the SD card (design.md §11.1).
 *
 * Core 1, before the guest starts: it is the one moment core 1 may write
 * to atom_t, because core 0 is waiting for it and running nothing
 * (design.md §4.2's ownership resumes as soon as roms_load returns).
 */
#ifndef PICO_ATOM_ROMS_H
#define PICO_ATOM_ROMS_H

#include <stdbool.h>
#include <stdint.h>

#include "atom.h"
#include "romset.h"

typedef enum {
    ROM_MISSING = 0,     /* no file */
    ROM_LOADED,          /* loaded, and the SHA-1 is the known image's */
    ROM_UNRECOGNISED,    /* loaded, but not the image §11.1 records */
    ROM_BAD_SIZE,        /* not 4 KiB: not loaded */
    ROM_SKIPPED,         /* present, but its hardware is not fitted */
} rom_state_t;

typedef struct {
    bool        card;            /* a card was found and mounted */
    int         mount_error;     /* FatFs FRESULT, 0 when mounted */
    rom_state_t slot[ROM_SLOT_COUNT];
    bool        from_ic20;       /* BASIC and kernel came from abasic.ic20 */
} roms_report_t;

/* Mount the card and load every slot it has into m. Returns true when
 * the machine can boot, which needs the required slots (romset.h). */
bool roms_load(atom_t *m, roms_report_t *r);

/* Log the report on stdio, one line per slot. */
void roms_log(const roms_report_t *r);

/* Fill an alpha-mode VRAM page explaining what is missing, for the
 * display to present instead of a dead machine (§11.1). */
void roms_explain(const roms_report_t *r, uint8_t *vram);

#endif /* PICO_ATOM_ROMS_H */
