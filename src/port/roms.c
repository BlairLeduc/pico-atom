/* roms.c — the ROM set off the SD card (design.md §11.1). */

#include "roms.h"

#include <stdio.h>
#include <string.h>

#include "ff.h"

#include "config.h"
#include "mc6847.h"
#include "sd.h"

#define ROM_DIR "/atom/roms/"

static FATFS   s_fs;
static FIL     s_file;
static uint8_t s_image[2 * ROM_IMAGE_SIZE];   /* room for abasic.ic20 */

/* Read a whole file of at most sizeof s_image bytes. Returns the length,
 * or -1 if it is missing or too big. */
static int read_file(const char *path) {
    if (f_open(&s_file, path, FA_READ) != FR_OK) return -1;
    UINT n = 0;
    FSIZE_t size = f_size(&s_file);
    FRESULT fr = (size <= sizeof s_image) ? f_read(&s_file, s_image, (UINT)size, &n) : FR_DENIED;
    f_close(&s_file);
    return (fr == FR_OK && n == size) ? (int)n : -1;
}

static rom_state_t load_slot(atom_t *m, int slot, const uint8_t *data) {
    atom_load_rom(m, romset_slots[slot].addr, data, ROM_IMAGE_SIZE);
    if (!romset_slots[slot].has_sha1) return ROM_LOADED;
    return romset_identify(data, ROM_IMAGE_SIZE) == slot ? ROM_LOADED : ROM_UNRECOGNISED;
}

bool roms_load(atom_t *m, roms_report_t *r) {
    memset(r, 0, sizeof(*r));

    r->mount_error = (int)f_mount(&s_fs, "", 1);
    r->card = (r->mount_error == FR_OK);
    if (!r->card) return false;

    for (int s = 0; s < ROM_SLOT_COUNT; s++) {
        char path[ATOM_PATH_MAX];
        snprintf(path, sizeof path, ROM_DIR "%s", romset_slots[s].file);
        int n = read_file(path);
        if (n < 0) { r->slot[s] = ROM_MISSING; continue; }
        if (n != (int)ROM_IMAGE_SIZE) { r->slot[s] = ROM_BAD_SIZE; continue; }

        /* AtomDOS wants the 8271, which is M9's (§17). Loading its ROM
         * without the controller gives a machine whose *DOS hangs. */
        if (s == ROM_DOS && !m->cfg.atomdos) { r->slot[s] = ROM_SKIPPED; continue; }
        r->slot[s] = load_slot(m, s, s_image);
    }

    /* MAME's 8 KiB abasic.ic20 is BASIC then the kernel (§11.1): the same
     * data split differently, so say so rather than reject it. */
    if (r->slot[ROM_BASIC] == ROM_MISSING || r->slot[ROM_KERNEL] == ROM_MISSING) {
        if (read_file(ROM_DIR "abasic.ic20") == (int)(2 * ROM_IMAGE_SIZE)) {
            r->slot[ROM_BASIC]  = load_slot(m, ROM_BASIC,  s_image);
            r->slot[ROM_KERNEL] = load_slot(m, ROM_KERNEL, s_image + ROM_IMAGE_SIZE);
            r->from_ic20 = true;
        }
    }

    f_unmount("");

    for (int s = 0; s < ROM_SLOT_COUNT; s++) {
        bool in = (r->slot[s] == ROM_LOADED || r->slot[s] == ROM_UNRECOGNISED);
        if (romset_slots[s].required && !in) return false;
    }
    return true;
}

static const char *state_name(rom_state_t st) {
    switch (st) {
    case ROM_LOADED:       return "OK";
    case ROM_UNRECOGNISED: return "UNKNOWN IMAGE";
    case ROM_BAD_SIZE:     return "NOT 4096 BYTES";
    case ROM_SKIPPED:      return "NOT USED";
    default:               return "MISSING";
    }
}

void roms_log(const roms_report_t *r) {
    if (!r->card) {
        printf("  roms         : %s (FatFs error %d)\n",
               sd_present() ? "card did not mount" : "no SD card", r->mount_error);
        return;
    }
    for (int s = 0; s < ROM_SLOT_COUNT; s++) {
        printf("  roms         : #%04X %-12s %s%s%s\n",
               romset_slots[s].addr, romset_slots[s].file, state_name(r->slot[s]),
               romset_slots[s].required ? "" : " (optional)",
               (r->from_ic20 && (s == ROM_BASIC || s == ROM_KERNEL)) ? " from abasic.ic20" : "");
    }
    for (int s = 0; s < ROM_SLOT_COUNT; s++) {
        if (r->slot[s] == ROM_UNRECOGNISED) {
            printf("  WARNING: %s is not the image design.md §11.1 records; a near-miss "
                   "boots and then misbehaves\n", romset_slots[s].file);
        }
    }
}

/* ---- the explanatory page --------------------------------------------- */

/* ASCII to the MC6847's glyph order (CLAUDE.md), upper case only. */
static uint8_t glyph(char c, bool inverse) {
    uint8_t a = (uint8_t)c;
    if (a >= 'a' && a <= 'z') a = (uint8_t)(a - 32);
    uint8_t g = (a >= 0x40u && a < 0x60u) ? (uint8_t)(a - 0x40u)
              : (a >= 0x20u && a < 0x40u) ? a : (uint8_t)' ';
    return inverse ? (uint8_t)(g | 0x40u) : g;
}

static void put(uint8_t *vram, int row, int col, const char *s, bool inverse) {
    for (; *s && col < 32; s++, col++) vram[row * 32 + col] = glyph(*s, inverse);
}

void roms_explain(const roms_report_t *r, uint8_t *vram) {
    memset(vram, glyph(' ', false), 512);
    put(vram, 0, 0, "        PICO-ATOM: NO ROMS      ", true);

    if (!r->card) {
        put(vram, 2, 0, sd_present() ? "THE SD CARD DID NOT MOUNT." : "NO SD CARD.", false);
        put(vram, 3, 0, "FAT32 OR FAT16, THEN RESET.", false);
    } else {
        put(vram, 2, 0, "ON THE SD CARD, IN /ATOM/ROMS/:", false);
        for (int s = 0; s < ROM_SLOT_COUNT; s++) {
            put(vram, 4 + s, 1, romset_slots[s].file, false);
            put(vram, 4 + s, 14, state_name(r->slot[s]), r->slot[s] != ROM_LOADED &&
                                  romset_slots[s].required);
        }
        put(vram, 10, 0, "AKERNEL AND ABASIC ARE NEEDED.", false);
    }
    put(vram, 13, 0, "README.MD SAYS WHERE TO GET THEM", false);
    put(vram, 14, 0, "AND HOW TO CHECK THEM.", false);
}
