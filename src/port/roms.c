/* roms.c — the ROM set off the SD card (design.md §11.1). */

#include "roms.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "ff.h"

#include "config.h"
#include "sd.h"
#include "settings.h"
#include "sha1.h"
#include "textpage.h"

#define ROM_DIR "/atom/roms/"

static FIL     s_file;
static uint8_t s_image[2 * ROM_IMAGE_SIZE];   /* room for abasic.ic20 */

#define FILE_MISSING  (-1)   /* could not be opened or read */
#define FILE_TOO_BIG  (-2)   /* bigger than any ROM this loader takes */

/* Read a whole file of at most sizeof s_image bytes. Returns the length,
 * FILE_MISSING or FILE_TOO_BIG. */
static int read_file(const char *path) {
    if (f_open(&s_file, path, FA_READ) != FR_OK) return FILE_MISSING;
    FSIZE_t size = f_size(&s_file);
    if (size > sizeof s_image) {
        f_close(&s_file);
        return FILE_TOO_BIG;
    }
    UINT n = 0;
    FRESULT fr = f_read(&s_file, s_image, (UINT)size, &n);
    f_close(&s_file);
    return (fr == FR_OK && n == size) ? (int)n : FILE_MISSING;
}

static rom_state_t load_slot(atom_t *m, roms_report_t *r, int slot, const uint8_t *data) {
    atom_load_rom(m, romset_slots[slot].addr, data, ROM_IMAGE_SIZE);
    sha1(data, ROM_IMAGE_SIZE, r->sha1[slot]);
    if (!romset_slots[slot].has_sha1) return ROM_LOADED;
    return romset_identify(data, ROM_IMAGE_SIZE) == slot ? ROM_LOADED : ROM_UNRECOGNISED;
}

/* The card path for a slot: §11.1's name, or the utility socket's
 * chosen file. False when the socket has none. */
static bool slot_path(int s, const char *utility, char path[ATOM_PATH_MAX]) {
    const char *name = s == ROM_UTILITY ? utility : romset_slots[s].file;
    if (!name || !name[0]) return false;
    int n = snprintf(path, ATOM_PATH_MAX, ROM_DIR "%s", name);
    return n > 0 && n < (int)ATOM_PATH_MAX;
}

bool roms_load(atom_t *m, const char *utility, roms_report_t *r) {
    memset(r, 0, sizeof(*r));
    r->card = true;
    snprintf(r->utility, sizeof r->utility, "%s", utility ? utility : "");

    for (int s = 0; s < ROM_SLOT_COUNT; s++) {
        char path[ATOM_PATH_MAX];
        if (!slot_path(s, utility, path)) { r->slot[s] = ROM_MISSING; continue; }
        int n = read_file(path);
        if (n == FILE_MISSING) { r->slot[s] = ROM_MISSING; continue; }
        if (n != (int)ROM_IMAGE_SIZE) { r->slot[s] = ROM_BAD_SIZE; continue; }

        /* AtomDOS wants the 8271: without the controller, *DOS
         * gives a machine that hangs on its first disc command. */
        if (s == ROM_DOS && !m->cfg.atomdos) { r->slot[s] = ROM_SKIPPED; continue; }
        r->slot[s] = load_slot(m, r, s, s_image);
    }

    /* MAME's 8 KiB abasic.ic20 is BASIC then the kernel (§11.1): the same
     * data split differently, so say so rather than reject it. */
    if (r->slot[ROM_BASIC] == ROM_MISSING || r->slot[ROM_KERNEL] == ROM_MISSING) {
        if (read_file(ROM_DIR "abasic.ic20") == (int)(2 * ROM_IMAGE_SIZE)) {
            r->slot[ROM_BASIC]  = load_slot(m, r, ROM_BASIC,  s_image);
            r->slot[ROM_KERNEL] = load_slot(m, r, ROM_KERNEL, s_image + ROM_IMAGE_SIZE);
            r->from_ic20 = true;
        }
    }

    for (int s = 0; s < ROM_SLOT_COUNT; s++) {
        bool in = (r->slot[s] == ROM_LOADED || r->slot[s] == ROM_UNRECOGNISED);
        if (romset_slots[s].required && !in) return false;
    }
    return true;
}

/* A file's name in the status row's upper case. */
static const char *upper(const char *name) {
    static char out[ROMS_NAME_MAX];
    size_t i = 0;
    for (; name[i] && i + 1 < sizeof out; i++) out[i] = (char)toupper((unsigned char)name[i]);
    out[i] = 0;
    return out;
}

const char *roms_check(const atom_config_t *cfg, const char *utility) {
    static char why[40];
    bool ic20 = false;
    for (int s = 0; s < ROM_SLOT_COUNT; s++) {
        char path[ATOM_PATH_MAX];
        if (s == ROM_DOS && !cfg->atomdos) continue;
        if (!slot_path(s, utility, path)) continue;
        int n = read_file(path);
        const char *name = s == ROM_UTILITY ? utility : romset_slots[s].file;
        if (n == FILE_MISSING) {
            if (romset_slots[s].required) {
                /* The one other way to have them (§11.1). */
                if (!ic20) ic20 = read_file(ROM_DIR "abasic.ic20") == (int)(2 * ROM_IMAGE_SIZE);
                if (ic20 && (s == ROM_KERNEL || s == ROM_BASIC)) continue;
            } else if (s != ROM_UTILITY || strcasecmp(name, SETTINGS_UTILITY) == 0) {
                continue;
            }
            snprintf(why, sizeof why, "%.20s MISSING", upper(name));
            return why;
        }
        if (n != (int)ROM_IMAGE_SIZE) {
            snprintf(why, sizeof why, "%.16s NOT 4096 BYTES", upper(name));
            return why;
        }
        /* Read and hashed, as the load will; nothing is kept. */
        uint8_t d[SHA1_DIGEST_LEN];
        sha1(s_image, ROM_IMAGE_SIZE, d);
    }
    return NULL;
}

/* §11.1's four fixed names are not utility ROMs. */
static bool fixed_name(const char *name) {
    for (int s = 0; s < ROM_SLOT_COUNT; s++) {
        if (s != ROM_UTILITY && strcasecmp(name, romset_slots[s].file) == 0) return true;
    }
    return false;
}

unsigned roms_list_utility(char names[][ROMS_NAME_MAX], unsigned max) {
    DIR dir;
    static FILINFO fi;
    unsigned n = 0;
    if (f_opendir(&dir, "/atom/roms") != FR_OK) return 0;
    while (n < max && f_readdir(&dir, &fi) == FR_OK && fi.fname[0]) {
        size_t len = strlen(fi.fname);
        if ((fi.fattrib & AM_DIR) || len < 5 || len >= ROMS_NAME_MAX) continue;
        if (strcasecmp(fi.fname + len - 4, ".rom") != 0 || fixed_name(fi.fname)) continue;
        memcpy(names[n++], fi.fname, len + 1);
    }
    f_closedir(&dir);
    /* Sorted, ignoring case: a handful of names, so an insertion sort. */
    for (unsigned i = 1; i < n; i++) {
        char t[ROMS_NAME_MAX];
        memcpy(t, names[i], sizeof t);
        unsigned j = i;
        for (; j > 0 && strcasecmp(names[j - 1], t) > 0; j--) memcpy(names[j], names[j - 1], sizeof t);
        memcpy(names[j], t, sizeof t);
    }
    return n;
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
        const char *file = s == ROM_UTILITY ? (r->utility[0] ? r->utility : "(none)")
                                            : romset_slots[s].file;
        printf("  roms         : #%04X %-12s %s%s%s\n",
               romset_slots[s].addr, file, state_name(r->slot[s]),
               romset_slots[s].required ? "" : " (optional)",
               (r->from_ic20 && (s == ROM_BASIC || s == ROM_KERNEL)) ? " from abasic.ic20" : "");
    }
    for (int s = 0; s < ROM_SLOT_COUNT; s++) {
        if (r->slot[s] == ROM_UNRECOGNISED) {
            printf("  WARNING: %s is not the image design.md §11.1 records; a near-miss "
                   "boots and then misbehaves\n", romset_slots[s].file);
        }
        if (r->slot[s] == ROM_LOADED || r->slot[s] == ROM_UNRECOGNISED) {
            printf("  roms         : #%04X sha1 ", romset_slots[s].addr);
            for (unsigned i = 0; i < SHA1_DIGEST_LEN; i++) printf("%02x", r->sha1[s][i]);
            printf("\n");
        }
    }
}

/* ---- the explanatory page --------------------------------------------- */

static void put(uint8_t *vram, int row, int col, const char *s, bool inverse) {
    textpage_put(vram, row, col, s, inverse);
}

void roms_explain(const roms_report_t *r, uint8_t *vram) {
    textpage_clear(vram);
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
