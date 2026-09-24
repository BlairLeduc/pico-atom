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

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "guest.h"
#include "mc6847.h"
#if PICO_ATOM_HAVE_FONT
#include "mc6847_font.h"
#endif
#include "test_util.h"

static guest_t g;
static atom_t  booted;      /* the machine at its first prompt */

/* Every field's audio, drained the way the port drains it (§12.2), and
 * kept from the last reset of audio_n so the bell can be measured. */
#define AUDIO_MAX 80000u
static int16_t audio[AUDIO_MAX];
static size_t  audio_n;

static void capture(atom_t *m) {
    int16_t tmp[ATOM_AUDIO_BUF_LEN];
    size_t n = atom_audio_drain(m, tmp, ATOM_AUDIO_BUF_LEN);
    if (audio_n + n <= AUDIO_MAX) {
        memcpy(audio + audio_n, tmp, n * sizeof(tmp[0]));
        audio_n += n;
    }
}

static void restore(void) {
    atom_copy(&g.m, &booted);
    keymatrix_init(&g.k);
}

static void fields(int n)                  { guest_fields(&g, n); }
static void settle(void)                   { guest_settle(&g); }
static void tap(uint8_t code)              { guest_tap(&g, code); }
static void chord(uint8_t mod, uint8_t c)  { guest_chord(&g, mod, c); }
static void type(const char *s)            { guest_type(&g, s); }
static uint8_t screen_code(char c)         { return guest_screen_code(c); }
static const char *row_text(int row)       { return guest_row(&g.m, row); }
static int cursor(void)                    { return guest_cursor(&g.m); }
static const uint8_t *vram(void)           { return atom_vram(&g.m); }

int main(void) {
    const char *dir;
    if (!guest_find_roms(&dir)) {
        printf("skipped: no kernel and BASIC images in %s\n", dir);
        return TEST_SKIP_CODE;
    }
    g.on_field = capture;

    /* ---- it boots to a prompt ----------------------------------------- */
    guest_boot(&g);
    CHECK(strcmp(row_text(0), "ACORN ATOM") == 0, "banner: '%s'", row_text(0));
    CHECK(strcmp(row_text(2), ">") == 0, "prompt: '%s'", row_text(2));
    CHECK(cursor() == 2 * 32 + 1, "cursor after the prompt, at %d", cursor());
    CHECK(atom_vdg_mode(&g.m) == 0, "alpha mode, got 0x%02X", atom_vdg_mode(&g.m));
    CHECK(g.m.cpu.undoc_count == 0, "undocumented opcode 0x%02X at #%04X",
          g.m.cpu.undoc_op, g.m.cpu.undoc_pc);
    atom_copy(&booted, &g.m);

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
        mc6847_set_mode(&vdg, atom_vdg_mode(&g.m));
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
        CHECK(g.m.ram[0x100] == 'A' && g.m.ram[0x101] == 'C',
              "COPY into the line buffer: %02X %02X", g.m.ram[0x100], g.m.ram[0x101]);

        /* CTRL: CTRL-L is form feed, which clears the screen. */
        restore();
        chord(PICOCALC_KEY_CTRL, 'l');
        CHECK(strcmp(row_text(0), "ACORN ATOM") != 0, "CTRL-L left: '%s'", row_text(0));

        /* REPT (Tab), held with a key, repeats it. */
        restore();
        keymatrix_event(&g.k, KEY_EV_PRESSED, 0x09u);
        keymatrix_event(&g.k, KEY_EV_PRESSED, 'a');
        fields(60);
        keymatrix_event(&g.k, KEY_EV_RELEASED, 'a');
        keymatrix_event(&g.k, KEY_EV_RELEASED, 0x09u);
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
        CHECK(g.k.menu_request, "Alt+M should request the menu");
        CHECK(strcmp(row_text(2), ">") == 0, "Alt+M reached the guest: '%s'", row_text(2));
    }

    /* ---- game keymaps (§10.5) ----------------------------------------- *
     * A game scans the matrix and the lines itself, so the test is the
     * guest doing that: a BASIC loop selects column 2 through port A and
     * copies ports B and C to #9000, past the alpha page in VRAM, while
     * the test holds keys under Games. Ports are active low. */
    {
        restore();
        type("DO?#B000=2;?#9000=?#B001;?#9001=?#B002;UNTIL0\n");
        keymatrix_set_layout(&g.k, &keylayout_builtin[0]);
        fields(10);
        uint8_t idle_b = g.m.ram[0x9000], idle_c = g.m.ram[0x9001];
        CHECK((idle_b & 0xC1u) == 0xC1u && (idle_c & 0x40u),
              "idle: port B 0x%02X, port C 0x%02X", idle_b, idle_c);

        static const struct {
            uint8_t code; const char *what; uint8_t b_low, c_low;
        } held[] = {
            { 0xB4u, "Left: UPDOWN, no SHIFT", 0x01u, 0 },
            { 0xB7u, "Right: CTRL alone",      0x40u, 0 },
            { ']',   "]: REPT",                0,     0x40u },
        };
        for (size_t i = 0; i < sizeof held / sizeof held[0]; i++) {
            keymatrix_event(&g.k, KEY_EV_PRESSED, held[i].code);
            fields(4);
            uint8_t b = g.m.ram[0x9000], c = g.m.ram[0x9001];
            CHECK((b & 0xC1u) == (0xC1u & ~held[i].b_low) &&
                      (c & 0x40u) == (0x40u & ~held[i].c_low),
                  "%s: port B 0x%02X, port C 0x%02X", held[i].what, b, c);
            keymatrix_event(&g.k, KEY_EV_RELEASED, held[i].code);
            fields(10);
        }

        /* Moving and firing at once. */
        keymatrix_event(&g.k, KEY_EV_PRESSED, 0xB4u);
        keymatrix_event(&g.k, KEY_EV_PRESSED, ']');
        fields(4);
        CHECK((g.m.ram[0x9000] & 0xC1u) == 0xC0u && !(g.m.ram[0x9001] & 0x40u),
              "Left and ]: port B 0x%02X, port C 0x%02X", g.m.ram[0x9000],
              g.m.ram[0x9001]);
        keymatrix_event(&g.k, KEY_EV_RELEASED, 0xB4u);
        keymatrix_event(&g.k, KEY_EV_RELEASED, ']');
        fields(10);

        /* A card layout onto the SHIFT line. Bouncing Babies moves left
         * on ?#B001=127, SHIFT and nothing else, and right on PC6, REPT. */
        static keylayout_t babies;
        const char *map = "left = SHIFT\nright = REPT\n";
        unsigned line = 0;
        CHECK(keylayout_parse(&babies, "babies", map, strlen(map), &line) == KL_OK,
              "the SHIFT/REPT layout parses, line %u", line);
        keymatrix_set_layout(&g.k, &babies);
        keymatrix_event(&g.k, KEY_EV_PRESSED, 0xB4u);
        fields(4);
        CHECK(g.m.ram[0x9000] == 0x7Fu && (g.m.ram[0x9001] & 0x40u),
              "Left as SHIFT: port B 0x%02X, port C 0x%02X", g.m.ram[0x9000],
              g.m.ram[0x9001]);
        keymatrix_event(&g.k, KEY_EV_RELEASED, 0xB4u);
        fields(10);
        keymatrix_event(&g.k, KEY_EV_PRESSED, 0xB7u);
        fields(4);
        CHECK((g.m.ram[0x9000] & 0x80u) && !(g.m.ram[0x9001] & 0x40u),
              "Right as REPT: port B 0x%02X, port C 0x%02X", g.m.ram[0x9000],
              g.m.ram[0x9001]);
        keymatrix_event(&g.k, KEY_EV_RELEASED, 0xB7u);
        fields(10);
        keymatrix_set_layout(&g.k, NULL);
    }

    /* ---- RND runs from the seed (§7.2) -------------------------------- *
     * BASIC's RND is a shift register at #08-#0C (#C986). The seed must
     * survive the reset to the prompt, and a zero seed, which is what a
     * zero-filled machine had, must be the control that gives 0 for ever. */
    {
        uint64_t seed = 0;
        for (unsigned i = 0; i < 5; i++) seed |= (uint64_t)booted.ram[0x08u + i] << (8u * i);
        CHECK(seed == GUEST_RND_SEED, "the seed should survive the reset, #08-#0C hold %010llX",
              (unsigned long long)seed);

        long a = 0, b = 0;
        restore();
        type("P.RND,RND\n");
        CHECK(sscanf(row_text(3), "%ld %ld", &a, &b) == 2 && a != 0 && b != 0 && a != b,
              "RND from the seed: '%s'", row_text(3));

        restore();
        memset(&g.m.ram[0x08], 0, 5);
        type("P.RND,RND\n");
        CHECK(sscanf(row_text(3), "%ld %ld", &a, &b) == 2 && a == 0 && b == 0,
              "RND from a zero seed should be 0: '%s'", row_text(3));
    }

    /* ---- the bell (§17 M5) ------------------------------------------- *
     * CTRL-G reaches the kernel's bell at #FD18, which toggles PC2 through
     * the 8255's BSR path: STA #B003 (4), DEX/BNE over X = 0, so 256 turns
     * (5 x 256 - 1), EOR (2), INY (2), BPL (3). That is 1290 cycles a half
     * period, 387.6 Hz, and Y runs from 5 to 128, so 123 half periods. */
    {
        restore();
        audio_n = 0;
        chord(PICOCALC_KEY_CTRL, 'g');
        fields(30);

        const double T = 2048.0 / 75.0;
        double first = -1, last = -1;
        unsigned rising = 0;
        size_t lit_first = 0, lit_last = 0;
        for (size_t i = 1; i < audio_n; i++) {
            if (audio[i] != 0 && lit_first == 0) lit_first = i;
            if (audio[i] != audio[i - 1] && abs(audio[i] - audio[i - 1]) > 1000) lit_last = i;
            if (audio[i - 1] < 0 && audio[i] >= 0) {
                double frac = (double)-audio[i - 1] / (double)(audio[i] - audio[i - 1]);
                double at = ((double)(i - 1) + frac) * T;
                if (first < 0) first = at;
                last = at;
                rising++;
            }
        }
        double hz = rising > 1 ? (rising - 1) * 1e6 / (last - first) : 0;
        double want = 1e6 / 2580.0;
        double ms = (double)(lit_last - lit_first) * T / 1000.0;
        printf("bell: %.2f Hz over %.1f ms, %u cycles; the loop counts %.2f Hz over %.1f ms\n",
               hz, ms, rising, want, 122 * 1290 / 1000.0);
        CHECK(rising >= 60 && rising <= 62, "the bell should be ~61 cycles, got %u", rising);
        CHECK(fabs(hz - want) / want < 1e-3, "the bell at %.2f Hz, expected %.2f Hz", hz, want);
        CHECK(strcmp(row_text(2), ">") == 0, "CTRL-G should not print: '%s'", row_text(2));
    }

    TEST_DONE();
}
