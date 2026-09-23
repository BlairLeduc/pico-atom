/* test_boot.c — the Atom boots, and the keyboard types (design.md §17 M4).
 *
 * Needs the user's ROM images, which the tree does not ship (§11.1), so
 * without them this reports as skipped. It looks in PICO_ATOM_ROMS if
 * that is set, else the repository's roms/ staging directory, and
 * recognises images by SHA-1 whatever they are called (romset.h).
 *
 * Everything here goes through the real MOS: the keymap is checked by
 * typing each entry at the prompt and reading what the kernel put in
 * VRAM, and keys arrive as southbridge events through keymatrix, paced
 * the way the device will pace them.
 */

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "atom.h"
#include "keymatrix.h"
#include "mc6847.h"
#if PICO_ATOM_HAVE_FONT
#include "mc6847_font.h"
#endif
#include "romset.h"
#include "test_util.h"

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

/* ---- the machine ---------------------------------------------------- */

static atom_t      m;
static keymatrix_t k;
static atom_t      booted;      /* the machine at its first prompt */

static void field(void) {
    keymatrix_field(&k, &m);
    atom_run_field(&m);
}

static void fields(int n) {
    for (int i = 0; i < n; i++) field();
}

static void boot(void) {
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&m, &cfg);
    keymatrix_init(&k);
    for (int s = 0; s < ROM_SLOT_COUNT; s++) {
        /* AtomDOS wants the 8271 (M9), so its ROM waits for it. */
        if (have[s] && s != ROM_DOS) {
            atom_load_rom(&m, romset_slots[s].addr, images[s], ROM_IMAGE_SIZE);
        }
    }
    atom_reset(&m);
    fields(120);
}

static void restore(void) {
    m = booted;
    keymatrix_init(&k);
}

/* The screen byte the MOS writes for an ASCII character: the MC6847's
 * glyph order puts #40-#5F first, and lower case is inverse (§2.4). */
static uint8_t screen_code(char c) {
    uint8_t a = (uint8_t)c;
    if (a < 0x40u) return a;
    if (a < 0x60u) return (uint8_t)(a - 0x40u);
    return (uint8_t)(a + 0x20u);
}

static const uint8_t *vram(void) { return atom_vram(&m); }

/* The text on one alpha row, as ASCII, with inverse and graphics as '#'. */
static const char *row_text(int row) {
    static char s[33];
    for (int c = 0; c < 32; c++) {
        uint8_t v = vram()[row * 32 + c];
        if (v == 0xA0u) v = 0x20u;    /* the cursor, an inverse space */
        uint8_t g = v & 0x3Fu;
        s[c] = (v & 0xC0u) ? '#' : (char)(g < 32 ? '@' + g : g);
    }
    s[32] = 0;
    for (int c = 31; c >= 0 && s[c] == ' '; c--) s[c] = 0;
    return s;
}

/* Run until every queued event has been replayed and every key is up,
 * then long enough for the MOS to act on the last one. */
static void settle(void) {
    for (int i = 0; i < 1000 && (k.q_len > 0 || k.n > 0); i++) field();
    fields(10);
}

/* A key as the MCU sends it: press and release in one poll, the worst
 * case, then the rest of the 30 Hz poll interval before the next. */
static void tap(uint8_t code) {
    keymatrix_event(&k, KEY_EV_PRESSED, code);
    keymatrix_event(&k, KEY_EV_RELEASED, code);
    fields(2);
}

/* With a host modifier held around it. */
static void chord(uint8_t mod, uint8_t code) {
    keymatrix_event(&k, KEY_EV_PRESSED, mod);
    tap(code);
    keymatrix_event(&k, KEY_EV_RELEASED, mod);
    settle();
}

/* Type a string as a PicoCalc user would: shifted characters arrive with
 * Shift held, and the release comes back unshifted when Shift is let go
 * first (hardware-notes.md §6.2), which keymatrix must see through. */
static void type(const char *s) {
    for (; *s; s++) {
        uint8_t c = (uint8_t)*s;
        bool shifted = strchr("!\"#$%&'()=<+*>?", c) != NULL;
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32);   /* unshifted = capitals */
        if (c == '\n') c = 0x0Au;
        if (!shifted) { tap(c); continue; }
        keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_SHIFT_L);
        keymatrix_event(&k, KEY_EV_PRESSED, c);
        keymatrix_event(&k, KEY_EV_RELEASED, PICOCALC_KEY_SHIFT_L);
        keymatrix_event(&k, KEY_EV_RELEASED, keymap_picocalc_canonical(c));
        fields(2);
    }
    settle();
}

static int cursor(void) {
    int at = -1, count = 0;
    for (int i = 0; i < 512; i++) {
        if (vram()[i] & 0x80u) { at = i; count++; }
    }
    return count == 1 ? at : -1;
}

int main(void) {
    const char *dir = getenv("PICO_ATOM_ROMS");
    scan_roms(dir ? dir : PICO_ATOM_DEFAULT_ROMS);
    if (!have[ROM_KERNEL] || !have[ROM_BASIC]) {
        printf("skipped: no kernel and BASIC images in %s\n",
               dir ? dir : PICO_ATOM_DEFAULT_ROMS);
        return TEST_SKIP_CODE;
    }

    /* ---- it boots to a prompt ----------------------------------------- */
    boot();
    CHECK(strcmp(row_text(0), "ACORN ATOM") == 0, "banner: '%s'", row_text(0));
    CHECK(strcmp(row_text(2), ">") == 0, "prompt: '%s'", row_text(2));
    CHECK(cursor() == 2 * 32 + 1, "cursor after the prompt, at %d", cursor());
    CHECK(atom_vdg_mode(&m) == 0, "alpha mode, got 0x%02X", atom_vdg_mode(&m));
    CHECK(m.cpu.undoc_count == 0, "undocumented opcode 0x%02X at #%04X",
          m.cpu.undoc_op, m.cpu.undoc_pc);
    booted = m;

    /* ---- the VRAM byte's wiring, as the ROMs use it (mc6847.h) ------ *
     * The MOS draws its cursor by setting bit 7 of the cell, so bit 7
     * must be INV, and the cursor must render lit: this is the check that
     * would have caught it being drawn as an empty graphics cell. */
    CHECK(vram()[2 * 32 + 1] == (VDG_BYTE_INV | 0x20u), "cursor byte 0x%02X",
          vram()[2 * 32 + 1]);
#if PICO_ATOM_HAVE_FONT
    {
        static mc6847_t vdg;
        uint16_t row[ATOM_SCREEN_W];
        mc6847_init(&vdg);
        mc6847_set_font(&vdg, font_6847);
        mc6847_set_mode(&vdg, atom_vdg_mode(&m));
        mc6847_render_row(&vdg, vram(), 2 * 12 + 5, row);   /* mid-cell */
        CHECK(row[8] == mc6847_palette[VDG_GREEN] && row[15] == mc6847_palette[VDG_GREEN],
              "the cursor cell should render as a solid block");
    }
#endif

    /* BASIC's CLEAR 0 is 64 x 48 semigraphics 6: #40 is an empty cell,
     * and PLOT sets one element bit per point, top-left bit 5 to
     * bottom-right bit 0, with y counting up from the bottom. */
    {
        static const struct { const char *plot; unsigned cell; uint8_t byte; } pts[] = {
            { "PLOT 13,0,47\n",  0,        0x60 },
            { "PLOT 13,1,47\n",  0,        0x50 },
            { "PLOT 13,0,46\n",  0,        0x48 },
            { "PLOT 13,1,46\n",  0,        0x44 },
            { "PLOT 13,0,45\n",  0,        0x42 },
            { "PLOT 13,1,45\n",  0,        0x41 },
            { "PLOT 13,0,44\n",  32,       0x60 },
            { "PLOT 13,63,0\n",  15 * 32 + 31, 0x41 },
        };
        for (size_t i = 0; i < sizeof pts / sizeof pts[0]; i++) {
            restore();
            type("CLEAR 0\n");
            CHECK(vram()[5 * 32 + 5] == VDG_BYTE_SG6, "CLEAR 0 fills with #40, got 0x%02X",
                  vram()[5 * 32 + 5]);
            type(pts[i].plot);
            CHECK(vram()[pts[i].cell] == pts[i].byte, "%.12s: cell %u is 0x%02X, want 0x%02X",
                  pts[i].plot, pts[i].cell, vram()[pts[i].cell], pts[i].byte);
        }
    }

    /* ---- M4's line: the prompt accepts PRINT 2+2 ---------------------- */
    restore();
    type("PRINT 2+2\n");
    CHECK(strcmp(row_text(2), ">PRINT 2+2") == 0, "typed: '%s'", row_text(2));
    /* Right-justified in the default field of eight (@=8), and Atom
     * BASIC's PRINT does not end the line — a ' does — so the next
     * prompt follows on the same row. */
    CHECK(strcmp(row_text(3), "       4>") == 0, "answer: '%s'", row_text(3));

    /* And a program, which exercises the floating-point ROM if present. */
    restore();
    type("10 FOR I=1 TO 3;P.I*I;N.\n20 END\nRUN\n");
    CHECK(strcmp(row_text(5), "       1       4       9>") == 0,
          "program output: '%s'", row_text(5));

    /* ---- every printing entry in the keymap types its character ----- */
    for (size_t i = 0; i < keymap_picocalc_len; i++) {
        const keymap_t *e = &keymap_picocalc[i];
        if (e->flags & (KM_ALT | KM_NOCELL)) continue;
        if (e->code < 0x20u || e->code > 0x7Eu) continue;

        restore();
        tap(e->code);
        settle();

        /* PicoCalc case is Atom case inverted (keymap_picocalc.c). */
        char want = (char)e->code;
        if (want >= 'a' && want <= 'z') want = (char)(want - 32);
        else if (want >= 'A' && want <= 'Z') want = (char)(want + 32);

        uint8_t got = vram()[2 * 32 + 1];
        CHECK(got == screen_code(want),
              "code 0x%02X at (%u,%u)%s: want screen 0x%02X, got 0x%02X",
              e->code, e->row, e->col, (e->flags & KM_SHIFT) ? "+SHIFT" : "",
              screen_code(want), got);
    }

    /* ---- the keys that do something other than print ---------------- */
    {
        restore();
        type("AB");
        tap(0x08u);                        /* Backspace -> DEL */
        settle();
        type("C");
        CHECK(strcmp(row_text(2), ">AC") == 0, "DEL: '%s'", row_text(2));

        /* The arrows, each from the prompt: one cell per pair, SHIFT
         * picking the direction (keymap_picocalc.c). */
        static const struct { uint8_t code; int move; const char *name; } arrows[] = {
            { 0xB5u, -32, "up" }, { 0xB6u, +32, "down" },
            { 0xB7u,  +1, "right" }, { 0xB4u, -1, "left" },
        };
        for (size_t i = 0; i < sizeof arrows / sizeof arrows[0]; i++) {
            restore();
            int home = cursor();
            tap(arrows[i].code);
            settle();
            CHECK(cursor() == home + arrows[i].move, "%s: cursor at %d from %d",
                  arrows[i].name, cursor(), home);
        }

        /* COPY: walk the cursor onto the banner and copy two letters.
         * The Atom has one cursor, so the screen does not change; the
         * characters go into the line buffer at #0100 and the cursor
         * advances over them. */
        restore();
        tap(0xB5u); tap(0xB5u); tap(0xB4u);
        settle();
        CHECK(cursor() == 0, "arrows to the banner: cursor at %d", cursor());
        chord(PICOCALC_KEY_ALT, 'C');
        chord(PICOCALC_KEY_ALT, 'C');
        CHECK(cursor() == 2, "COPY advances: cursor at %d", cursor());
        CHECK(m.ram[0x100] == 'A' && m.ram[0x101] == 'C',
              "COPY into the line buffer: %02X %02X", m.ram[0x100], m.ram[0x101]);

        /* CTRL: CTRL-L is form feed, which clears the screen. */
        restore();
        chord(PICOCALC_KEY_CTRL, 'l');
        CHECK(strcmp(row_text(0), "ACORN ATOM") != 0, "CTRL-L left: '%s'", row_text(0));

        /* REPT, held with a key, repeats it. */
        restore();
        keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_ALT);
        keymatrix_event(&k, KEY_EV_PRESSED, 'R');
        keymatrix_event(&k, KEY_EV_RELEASED, PICOCALC_KEY_ALT);
        keymatrix_event(&k, KEY_EV_PRESSED, 'a');
        fields(60);
        keymatrix_event(&k, KEY_EV_RELEASED, 'a');
        keymatrix_event(&k, KEY_EV_RELEASED, 'R');
        fields(10);
        CHECK(strncmp(row_text(2), ">AAAA", 5) == 0, "REPT: '%s'", row_text(2));

        /* BREAK: the MOS starts again from its reset vector, and the
         * typing is gone. */
        restore();
        type("XYZ");
        chord(PICOCALC_KEY_ALT, 'K');
        fields(60);
        CHECK(strcmp(row_text(0), "ACORN ATOM") == 0, "BREAK banner: '%s'", row_text(0));
        CHECK(strcmp(row_text(2), ">") == 0, "BREAK prompt: '%s'", row_text(2));

        /* LOCK turns the letters over: unshifted A is then lower case. */
        restore();
        chord(PICOCALC_KEY_ALT, 'L');
        type("A");
        CHECK(vram()[2 * 32 + 1] == screen_code('a'),
              "LOCK: got 0x%02X", vram()[2 * 32 + 1]);

        /* The menu chord reaches the UI, not the guest. */
        restore();
        chord(PICOCALC_KEY_ALT, 'M');
        CHECK(k.menu_request, "Alt+M should request the menu");
        CHECK(strcmp(row_text(2), ">") == 0, "Alt+M reached the guest: '%s'", row_text(2));
    }

    TEST_DONE();
}
