/* romset.h — the ROM sockets and the images known to fit them
 * (design.md §11.1).
 *
 * The device finds each image by the file name §11.1 gives it and then
 * checks it by SHA-1: Atom ROMs have been re-dumped and renamed for forty
 * years, and a near-miss boots and then misbehaves, which §16 calls the
 * most expensive class of bug here. The host tests go further and pick
 * images out of a directory by hash alone. The table is data about
 * images; nothing in the tree contains one.
 */
#ifndef PICO_ATOM_ROMSET_H
#define PICO_ATOM_ROMSET_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "sha1.h"

#define ROM_IMAGE_SIZE 4096u

typedef enum {
    ROM_KERNEL,     /* #F000, the MOS and the 6502 vectors */
    ROM_BASIC,      /* #C000 */
    ROM_FLOAT,      /* #D000 */
    ROM_DOS,        /* #E000 */
    ROM_UTILITY,    /* #A000, the user's choice */
    ROM_SLOT_COUNT,
    ROM_UNKNOWN = -1,
} rom_slot_t;

typedef struct {
    const char *file;        /* its name under /atom/roms/ */
    uint16_t    addr;
    bool        required;    /* no machine without it */
    bool        has_sha1;    /* the utility socket takes any image */
    uint8_t     sha1[SHA1_DIGEST_LEN];
} rom_slot_info_t;

extern const rom_slot_info_t romset_slots[ROM_SLOT_COUNT];

/* Which slot's known image is this 4 KiB? ROM_UNKNOWN for anything
 * else, including the right size with the wrong bytes. An 8 KiB
 * abasic.ic20 is two images, BASIC then kernel (§11.1): identify each
 * half. */
rom_slot_t romset_identify(const uint8_t *data, size_t len);

#endif /* PICO_ATOM_ROMSET_H */
