/* roms.h — load the ROM set off the SD card (design.md §11.1, §13.1).
 *
 * Core 1, with core 0 not running the guest: at boot core 0 is waiting,
 * and for the Machine page's restart it is parked (§4.2). Those are the
 * moments core 1 may write to atom_t.
 */
#ifndef PICO_ATOM_ROMS_H
#define PICO_ATOM_ROMS_H

#include <stdbool.h>
#include <stdint.h>

#include "atom.h"
#include "romset.h"

/* A utility ROM's file name, as the Machine page lists it (§13.1). */
#define ROMS_NAME_MAX 32u

typedef enum {
    ROM_MISSING = 0,     /* no file, or none chosen */
    ROM_LOADED,          /* loaded; for a slot with a known image, the SHA-1 is it */
    ROM_UNRECOGNISED,    /* loaded, but not the image §11.1 records */
    ROM_BAD_SIZE,        /* not 4 KiB: not loaded */
    ROM_SKIPPED,         /* present, but its hardware is not fitted */
} rom_state_t;

typedef struct {
    bool        card;            /* a card was found and mounted */
    int         mount_error;     /* FatFs FRESULT, 0 when mounted */
    rom_state_t slot[ROM_SLOT_COUNT];
    /* The SHA-1 of each image loaded, for the About page (§13.1). */
    uint8_t     sha1[ROM_SLOT_COUNT][SHA1_DIGEST_LEN];
    /* The file in the utility socket at #A000, "" for none. */
    char        utility[ROMS_NAME_MAX];
    bool        from_ic20;       /* BASIC and kernel came from abasic.ic20 */
} roms_report_t;

/* Load every slot the card has into m, the utility socket from
 * `utility`, a file in /atom/roms/ or "" for none. The card must be
 * mounted. Returns true when the machine can boot, which needs the
 * required slots (romset.h). */
bool roms_load(atom_t *m, const char *utility, roms_report_t *r);

/* The first pass of a restart (§13.1): read and hash every file a
 * machine with `cfg` and `utility` needs, touching no machine. NULL when
 * all is well, else why not, for the status row: "AKERNEL.ROM MISSING".
 * A required image missing, any of the wrong size, or a utility ROM that
 * the settings name and the card lacks refuses; SETTINGS_UTILITY missing
 * is an empty socket, as at boot. The card must be mounted. */
const char *roms_check(const atom_config_t *cfg, const char *utility);

/* The utility ROMs the Machine page offers: the .rom files in
 * /atom/roms/ less the four names §11.1 fixes, sorted, at most `max`.
 * The card must be mounted. */
unsigned roms_list_utility(char names[][ROMS_NAME_MAX], unsigned max);

/* Log the report on stdio, one line per slot. */
void roms_log(const roms_report_t *r);

/* Fill an alpha-mode VRAM page explaining what is missing, for the
 * display to present instead of a dead machine (§11.1). */
void roms_explain(const roms_report_t *r, uint8_t *vram);

#endif /* PICO_ATOM_ROMS_H */
