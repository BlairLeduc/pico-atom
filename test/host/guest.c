/* guest.c — a real Atom, on the host (guest.h). */

#include "guest.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint8_t images[ROM_SLOT_COUNT][ROM_IMAGE_SIZE];
static bool    have[ROM_SLOT_COUNT];

static void consider(const uint8_t *data, size_t len) {
    for (size_t off = 0; off + ROM_IMAGE_SIZE <= len; off += ROM_IMAGE_SIZE) {
        rom_slot_t s = romset_identify(data + off, ROM_IMAGE_SIZE);
        if (s == ROM_UNKNOWN) continue;
        memcpy(images[s], data + off, ROM_IMAGE_SIZE);
        have[s] = true;
    }
}

static void scan_roms(const char *dir) {
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *e;
    while ((e = readdir(d)) != NULL) {
        char path[1024];
        snprintf(path, sizeof path, "%s/%s", dir, e->d_name);
        FILE *f = fopen(path, "rb");
        if (!f) continue;
        static uint8_t buf[2 * ROM_IMAGE_SIZE + 1];
        size_t n = fread(buf, 1, sizeof buf, f);
        fclose(f);
        /* 4 KiB images, or an 8 KiB abasic.ic20 (§11.1). */
        if (n == ROM_IMAGE_SIZE || n == 2 * ROM_IMAGE_SIZE) consider(buf, n);
    }
    closedir(d);
}

bool guest_find_roms(const char **dir) {
    const char *env = getenv("PICO_ATOM_ROMS");
    const char *where = env ? env : PICO_ATOM_DEFAULT_ROMS;
    if (dir) *dir = where;
    scan_roms(where);
    return have[ROM_KERNEL] && have[ROM_BASIC];
}

bool guest_have_rom(rom_slot_t s) {
    return s >= 0 && s < ROM_SLOT_COUNT && have[s];
}

void guest_fields(guest_t *g, int n) {
    for (int i = 0; i < n; i++) {
        keymatrix_field(&g->k, &g->m);
        atom_run_field(&g->m);
        if (g->on_field) {
            g->on_field(&g->m);
        } else {
            int16_t discard[ATOM_AUDIO_BUF_LEN];
            (void)atom_audio_drain(&g->m, discard, ATOM_AUDIO_BUF_LEN);
        }
    }
}

void guest_boot(guest_t *g) {
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&g->m, &cfg);
    keymatrix_init(&g->k);
    for (int s = 0; s < ROM_SLOT_COUNT; s++) {
        /* AtomDOS wants the 8271 (M9), so its ROM waits for it. */
        if (have[s] && s != ROM_DOS) {
            atom_load_rom(&g->m, romset_slots[s].addr, images[s], ROM_IMAGE_SIZE);
        }
    }
    atom_reset(&g->m);
    guest_fields(g, 120);
}

void guest_settle(guest_t *g) {
    for (int i = 0; i < 1000 && (g->k.q_len > 0 || g->k.n > 0); i++) guest_fields(g, 1);
    guest_fields(g, 10);
}

void guest_tap(guest_t *g, uint8_t code) {
    keymatrix_event(&g->k, KEY_EV_PRESSED, code);
    keymatrix_event(&g->k, KEY_EV_RELEASED, code);
    guest_fields(g, 2);
}

void guest_chord(guest_t *g, uint8_t mod, uint8_t code) {
    keymatrix_event(&g->k, KEY_EV_PRESSED, mod);
    guest_tap(g, code);
    keymatrix_event(&g->k, KEY_EV_RELEASED, mod);
    guest_settle(g);
}

/* Shifted characters arrive with Shift held, and the release comes back
 * unshifted when Shift is let go first (hardware-notes.md §6.2), which
 * keymatrix must see through. */
void guest_type(guest_t *g, const char *s) {
    for (; *s; s++) {
        /* A typist, not a paste: wait for the replay to catch up, as a
         * PicoCalc user waits to see characters appear. */
        while (g->k.q_len + g->k.n_open + 8u > ATOM_KEY_EVENT_QUEUE) guest_fields(g, 1);
        uint8_t c = (uint8_t)*s;
        bool shifted = strchr("!\"#$%&'()=<+*>?", c) != NULL;
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);   /* unshifted = capitals */
        if (c == '\n') c = 0x0Au;
        if (!shifted) { guest_tap(g, c); continue; }
        keymatrix_event(&g->k, KEY_EV_PRESSED, PICOCALC_KEY_SHIFT_L);
        keymatrix_event(&g->k, KEY_EV_PRESSED, c);
        keymatrix_event(&g->k, KEY_EV_RELEASED, PICOCALC_KEY_SHIFT_L);
        keymatrix_event(&g->k, KEY_EV_RELEASED, keymap_picocalc_canonical(c));
        guest_fields(g, 2);
    }
    guest_settle(g);
}

/* The MC6847's glyph order puts #40-#5F first, and lower case is
 * inverse (§2.4). */
uint8_t guest_screen_code(char c) {
    uint8_t a = (uint8_t)c;
    if (a < 0x40u) return a;
    if (a < 0x60u) return (uint8_t)(a - 0x40u);
    return (uint8_t)(a + 0x20u);
}

const char *guest_row(const atom_t *m, int row) {
    static char s[33];
    const uint8_t *vram = atom_vram(m);
    for (int c = 0; c < 32; c++) {
        uint8_t v = vram[row * 32 + c];
        if (v == 0xA0u) v = 0x20u;    /* the cursor, an inverse space */
        uint8_t g = v & 0x3Fu;
        s[c] = (v & 0xC0u) ? '#' : (char)(g < 32 ? '@' + g : g);
    }
    s[32] = 0;
    for (int c = 31; c >= 0 && s[c] == ' '; c--) s[c] = 0;
    return s;
}

int guest_cursor(const atom_t *m) {
    const uint8_t *vram = atom_vram(m);
    int at = -1, count = 0;
    for (int i = 0; i < 512; i++) {
        if (vram[i] & 0x80u) { at = i; count++; }
    }
    return count == 1 ? at : -1;
}
