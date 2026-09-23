/* test_keymap.c — the keymap table, the held-key set, and ROM hashing
 * (design.md §10, §11.1). Needs no ROM; test_boot checks the same table
 * against the MOS itself when the images are present.
 */

#include <stdio.h>
#include <string.h>

#include "atom.h"
#include "keymatrix.h"
#include "romset.h"
#include "sha1.h"
#include "test_util.h"

static atom_t      m;
static keymatrix_t k;

static void fresh(void) {
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&m, &cfg);
    keymatrix_init(&k);
}

static bool cell_down(uint8_t row, uint8_t col) {
    return (m.key_col[col] >> row) & 1u;
}

static const keymap_t *entry_for(uint8_t code, bool alt) {
    for (size_t i = 0; i < keymap_picocalc_len; i++) {
        const keymap_t *e = &keymap_picocalc[i];
        if (e->code == code && !!(e->flags & KM_ALT) == alt) return e;
    }
    return NULL;
}

static bool hex_digest_is(const char *msg, size_t len, const char *hex) {
    uint8_t d[SHA1_DIGEST_LEN];
    sha1(msg, len, d);
    char got[2 * SHA1_DIGEST_LEN + 1];
    for (unsigned i = 0; i < SHA1_DIGEST_LEN; i++) {
        static const char digits[] = "0123456789abcdef";
        got[2 * i] = digits[d[i] >> 4];
        got[2 * i + 1] = digits[d[i] & 15u];
    }
    got[2 * SHA1_DIGEST_LEN] = 0;
    return strcmp(got, hex) == 0;
}

int main(void) {
    /* ---- SHA-1: the FIPS 180 examples, and the padding edges -------- */
    CHECK(hex_digest_is("", 0, "da39a3ee5e6b4b0d3255bfef95601890afd80709"), "empty");
    CHECK(hex_digest_is("abc", 3, "a9993e364706816aba3e25717850c26c9cd0d89d"), "abc");
    {
        const char *two = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        CHECK(hex_digest_is(two, strlen(two),
                            "84983e441c3bd26ebaae4aa1f95129e5e54670f1"),
              "the two-block message");
    }
    {
        /* Streaming in odd pieces must equal one shot. */
        static uint8_t buf[ROM_IMAGE_SIZE];
        for (unsigned i = 0; i < sizeof buf; i++) buf[i] = (uint8_t)(i * 7u + 3u);
        uint8_t one[SHA1_DIGEST_LEN], parts[SHA1_DIGEST_LEN];
        sha1(buf, sizeof buf, one);
        sha1_t s;
        sha1_init(&s);
        for (unsigned off = 0, step = 1; off < sizeof buf; off += step, step = step * 3u % 97u + 1u) {
            unsigned n = (off + step > sizeof buf) ? (unsigned)sizeof buf - off : step;
            sha1_update(&s, buf + off, n);
        }
        sha1_final(&s, parts);
        CHECK(memcmp(one, parts, SHA1_DIGEST_LEN) == 0, "streamed digest differs");
        CHECK(romset_identify(buf, sizeof buf) == ROM_UNKNOWN,
              "an arbitrary 4 KiB is not a known ROM");
        CHECK(romset_identify(buf, 100) == ROM_UNKNOWN, "wrong size is unknown");
    }

    /* ---- the ROM table agrees with §11.1's map ---------------------- */
    CHECK(romset_slots[ROM_KERNEL].addr == 0xF000u, "kernel at #F000");
    CHECK(romset_slots[ROM_BASIC].addr  == 0xC000u, "BASIC at #C000");
    CHECK(romset_slots[ROM_FLOAT].addr  == 0xD000u, "float at #D000");
    CHECK(romset_slots[ROM_DOS].addr    == 0xE000u, "DOS at #E000");
    CHECK(romset_slots[ROM_UTILITY].addr == 0xA000u, "utility at #A000");
    CHECK(romset_slots[ROM_KERNEL].required && romset_slots[ROM_BASIC].required,
          "the kernel and BASIC are what a machine needs");

    /* Every digest in the table is README.md's, byte for byte. The table
     * is typed in by hand as bytes, the README as hex, and a transposed
     * pair would only show when that ROM failed to be recognised — for
     * dosrom.rom, not until M9. */
    {
        FILE *f = fopen(PICO_ATOM_README, "r");
        CHECK(f != NULL, "cannot open %s", PICO_ATOM_README);
        unsigned checked = 0;
        char line[256];
        while (f && fgets(line, sizeof line, f)) {
            char hex[41], name[64];
            if (sscanf(line, "%40[0-9a-f]  %63s", hex, name) != 2 || strlen(hex) != 40)
                continue;
            for (int slot = 0; slot < ROM_SLOT_COUNT; slot++) {
                const rom_slot_info_t *r = &romset_slots[slot];
                if (!r->has_sha1 || strcmp(r->file, name) != 0) continue;
                char got[41];
                for (unsigned i = 0; i < SHA1_DIGEST_LEN; i++)
                    snprintf(&got[2 * i], 3, "%02x", r->sha1[i]);
                CHECK(strcmp(got, hex) == 0, "%s: table has %s, README.md has %s",
                      name, got, hex);
                checked++;
            }
        }
        if (f) fclose(f);
        CHECK(checked == 4, "README.md should list all four hashed slots, found %u",
              checked);
    }

    /* ---- keymap table invariants (§10.3) ----------------------------- */
    for (size_t i = 0; i < keymap_picocalc_len; i++) {
        const keymap_t *e = &keymap_picocalc[i];
        if (!(e->flags & KM_NOCELL)) {
            CHECK(e->row < ATOM_KEY_ROWS && e->col < ATOM_KEY_COLS,
                  "code 0x%02X: cell (%u,%u) off the matrix", e->code, e->row, e->col);
            /* The five cells with no key on the Atom (keymap_picocalc.c). */
            bool keyless = (e->row == 0 && e->col <= 1) || (e->row == 1 && e->col >= 7);
            CHECK(!keyless, "code 0x%02X bound to keyless cell (%u,%u)",
                  e->code, e->row, e->col);
        }
        /* The MCU keeps Alt+, . Space and B for itself (hardware-notes
         * §6.3), so an Alt binding on them could never fire. */
        if (e->flags & KM_ALT) {
            CHECK(strchr(",. B", e->code) == NULL,
                  "Alt+'%c' is consumed by the MCU", e->code);
            CHECK(e->code >= 'A' && e->code <= 'Z',
                  "Alt+0x%02X: Alt letters arrive in capitals", e->code);
        }
        for (size_t j = i + 1; j < keymap_picocalc_len; j++) {
            const keymap_t *f = &keymap_picocalc[j];
            CHECK(!(e->code == f->code && (e->flags & KM_ALT) == (f->flags & KM_ALT)),
                  "code 0x%02X bound twice", e->code);
        }
    }
    CHECK(entry_for(0xB4u, false) && (entry_for(0xB4u, false)->flags & KM_SHIFT),
          "left is the shifted half of its cell");
    CHECK(entry_for(0xB7u, false) && !(entry_for(0xB7u, false)->flags & KM_SHIFT),
          "right is the unshifted half");

    /* Canonical identity is idempotent, and maps both halves of a key
     * to one physical key. */
    for (unsigned c = 0; c < 256; c++) {
        uint8_t once = keymap_picocalc_canonical((uint8_t)c);
        CHECK(keymap_picocalc_canonical(once) == once, "canonical(0x%02X) not stable", c);
    }
    CHECK(keymap_picocalc_canonical('A') == 'a', "A is a");
    CHECK(keymap_picocalc_canonical('!') == '1', "! is 1");
    CHECK(keymap_picocalc_canonical(':') == ';', ": is ;");

    /* ---- the held set: pacing (§10.2) -------------------------------- */
    {
        /* Press and release in one poll: the key is down for exactly
         * ATOM_KEY_MIN_FIELDS fields. */
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, 'a');
        keymatrix_event(&k, KEY_EV_RELEASED, 'a');
        unsigned down = 0;
        for (int f = 0; f < 20; f++) {
            keymatrix_field(&k, &m);
            if (cell_down(3, 6)) down++;
        }
        CHECK(down == ATOM_KEY_MIN_FIELDS, "held %u fields, want %u", down,
              (unsigned)ATOM_KEY_MIN_FIELDS);
        CHECK(!m.key_shift, "a is unshifted");

        /* Two keys in one poll are serialised with the gap between. */
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, 'a');
        keymatrix_event(&k, KEY_EV_RELEASED, 'a');
        keymatrix_event(&k, KEY_EV_PRESSED, 'b');
        keymatrix_event(&k, KEY_EV_RELEASED, 'b');
        int a_last = -1, b_first = -1;
        for (int f = 0; f < 40; f++) {
            keymatrix_field(&k, &m);
            CHECK(!(cell_down(3, 6) && cell_down(3, 5)), "a and b overlap at field %d", f);
            if (cell_down(3, 6)) a_last = f;
            if (cell_down(3, 5) && b_first < 0) b_first = f;
        }
        CHECK(b_first - a_last - 1 == (int)ATOM_KEY_GAP_FIELDS,
              "gap of %d fields, want %u", b_first - a_last - 1,
              (unsigned)ATOM_KEY_GAP_FIELDS);

        /* A key held by a human is held here: auto-repeat presses do not
         * release it, and it goes up only when released. */
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, 'j');
        for (int f = 0; f < 30; f++) {
            if (f % 6 == 0) keymatrix_event(&k, KEY_EV_PRESSED, 'j');
            keymatrix_field(&k, &m);
            CHECK(cell_down(4, 7), "j let go at field %d while held", f);
        }
        keymatrix_event(&k, KEY_EV_RELEASED, 'j');
        keymatrix_field(&k, &m);
        CHECK(!cell_down(4, 7), "j should be up after its release");
        CHECK(k.n == 0, "nothing should be held");
    }

    /* ---- the shifted-release quirk (hardware-notes.md §6.2) ---------- */
    {
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_SHIFT_L);
        keymatrix_event(&k, KEY_EV_PRESSED, '"');
        keymatrix_event(&k, KEY_EV_RELEASED, PICOCALC_KEY_SHIFT_L);
        keymatrix_field(&k, &m);
        CHECK(cell_down(1, 1) && m.key_shift, "\" is SHIFT+2 on the Atom");
        keymatrix_event(&k, KEY_EV_RELEASED, '\'');   /* retranslated */
        for (int f = 0; f < 10; f++) keymatrix_field(&k, &m);
        CHECK(k.n == 0 && !cell_down(1, 1) && !m.key_shift,
              "a release under the other translation must still let go");

        /* ':' needs the host's Shift and the Atom's none. */
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_SHIFT_R);
        keymatrix_event(&k, KEY_EV_PRESSED, ':');
        keymatrix_field(&k, &m);
        CHECK(cell_down(2, 3) && !m.key_shift, ": is unshifted on the Atom");
    }

    /* ---- modifiers, the Alt layer and the lines that are not cells --- */
    {
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_CTRL);
        keymatrix_event(&k, KEY_EV_PRESSED, 'g');
        keymatrix_field(&k, &m);
        CHECK(m.key_ctrl && cell_down(3, 0), "CTRL-G");
        CHECK((m.ppi.in_b & 0x40u) == 0, "CTRL pulls port B bit 6 low");
        keymatrix_event(&k, KEY_EV_RELEASED, PICOCALC_KEY_CTRL);
        keymatrix_field(&k, &m);
        CHECK(!m.key_ctrl, "CTRL released");

        /* Alt+C is COPY, not a shifted C. */
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_ALT);
        keymatrix_event(&k, KEY_EV_PRESSED, 'C');
        keymatrix_field(&k, &m);
        CHECK(cell_down(1, 5) && !cell_down(3, 4) && !m.key_shift, "Alt+C is COPY");

        /* An Alt chord with no binding reaches nothing. */
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_ALT);
        keymatrix_event(&k, KEY_EV_PRESSED, 'Q');
        keymatrix_field(&k, &m);
        CHECK(k.n == 0 && !cell_down(4, 0), "Alt+Q should do nothing");

        /* Tab is the REPT line, active low on port C bit 6, and a key
         * pressed while it is held still reaches its own cell. */
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, 0x09u);
        keymatrix_event(&k, KEY_EV_PRESSED, 'q');
        keymatrix_field(&k, &m);
        CHECK(m.key_rept && (m.ppi.in_c & I8255_IN_C_REPT) == 0, "Tab is REPT");
        CHECK(cell_down(4, 0), "Q held with REPT reaches its cell");

        /* Shift+Tab arrives as Home, and is REPT too. */
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, 0xD2u);
        keymatrix_field(&k, &m);
        CHECK(m.key_rept, "Shift+Tab (Home) is REPT");

        /* Alt+K is BREAK, the reset line. */
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_ALT);
        keymatrix_event(&k, KEY_EV_PRESSED, 'K');
        m.cpu.reset_pending = false;
        keymatrix_field(&k, &m);
        CHECK(m.cpu.reset_pending, "Alt+K should reset the 6502");

        /* Alt+M asks for the menu and touches nothing in the machine. */
        fresh();
        keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_ALT);
        keymatrix_event(&k, KEY_EV_PRESSED, 'M');
        keymatrix_field(&k, &m);
        CHECK(k.menu_request, "Alt+M requests the menu");
        bool any = false;
        for (unsigned c = 0; c < ATOM_KEY_COLS; c++) any |= m.key_col[c] != 0;
        CHECK(!any && !m.key_shift && !m.key_rept, "Alt+M reached the matrix");
    }

    /* ---- the queue is bounded, and never loses a release ------------- */
    {
        /* Auto-repeat of a key already down is absorbed at once. */
        fresh();
        for (unsigned i = 0; i < ATOM_KEY_EVENT_QUEUE + 5u; i++) {
            keymatrix_event(&k, KEY_EV_PRESSED, 'x');
        }
        CHECK(k.q_len == 1 && k.dropped == 0, "repeats: q_len %u, dropped %u",
              k.q_len, (unsigned)k.dropped);
        keymatrix_event(&k, KEY_EV_RELEASED, 'x');

        /* A burst far faster than the replay: presses are refused once
         * the queue is short of room, but every key that went down comes
         * back up, so nothing is left held. */
        fresh();
        static const char burst[] = "the quick brown fox jumps over the lazy dog 0123456789";
        for (int rep = 0; rep < 3; rep++) {
            for (const char *c = burst; *c; c++) {
                keymatrix_event(&k, KEY_EV_PRESSED, (uint8_t)*c);
                keymatrix_event(&k, KEY_EV_RELEASED, (uint8_t)*c);
            }
        }
        CHECK(k.dropped > 0, "the burst should have overflowed");
        CHECK(k.q_len + k.n_open <= ATOM_KEY_EVENT_QUEUE, "a release has no room");
        for (int f = 0; f < 2000 && (k.q_len || k.n); f++) keymatrix_field(&k, &m);
        CHECK(k.q_len == 0 && k.n == 0 && k.n_open == 0, "stuck keys: %u held, %u open",
              k.n, k.n_open);

        /* The same, with Shift and Alt in the burst: a modifier's release
         * is kept like any other. */
        fresh();
        for (int i = 0; i < 100; i++) {
            keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_ALT);
            keymatrix_event(&k, KEY_EV_PRESSED, 'a');
            keymatrix_event(&k, KEY_EV_RELEASED, 'a');
            keymatrix_event(&k, KEY_EV_RELEASED, PICOCALC_KEY_ALT);
        }
        for (int f = 0; f < 2000 && (k.q_len || k.n); f++) keymatrix_field(&k, &m);
        CHECK(!k.alt && k.n == 0 && k.n_open == 0, "Alt left down after a burst");
    }

    /* ---- game keymaps: the built-in layouts (§10.5) ------------------- */
    CHECK(keylayout_builtin_len >= 1 && strcmp(keylayout_builtin[0].name, "GAMES") == 0,
          "Games is the first built-in layout");
    for (size_t li = 0; li < keylayout_builtin_len; li++) {
        const keylayout_t *l = &keylayout_builtin[li];
        CHECK(l->n <= ATOM_KEYMAP_BINDINGS && l->n_tapes <= ATOM_KEYMAP_TAPES,
              "%s: over config.h's capacities", l->name);
        for (unsigned i = 0; i < l->n; i++) {
            const keymap_t *e = &l->bind[i];
            CHECK(keymap_picocalc_canonical(e->code) == e->code,
                  "%s: 0x%02X is not a canonical code, so it would never match", l->name,
                  e->code);
            CHECK(!(e->flags & (KM_ALT | KM_BREAK | KM_MENU)),
                  "%s: 0x%02X takes something a layout may not", l->name, e->code);
            if (!(e->flags & KM_NOCELL)) {
                CHECK(!(e->flags & KM_SHIFT), "%s: a cell target carries SHIFT", l->name);
                CHECK(e->row < ATOM_KEY_ROWS && e->col < ATOM_KEY_COLS, "%s: off the matrix",
                      l->name);
            }
            for (unsigned j = i + 1; j < l->n; j++) {
                CHECK(l->bind[j].code != e->code, "%s: 0x%02X bound twice", l->name, e->code);
            }
        }
    }

    /* ---- game keymaps: the overlay in the held set -------------------- */
    {
        const keylayout_t *cursor = &keylayout_builtin[0];

        /* Left is the UPDOWN cell, SHIFT up: the game sees the cell. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        keymatrix_event(&k, KEY_EV_PRESSED, 0xB4u);
        keymatrix_field(&k, &m);
        CHECK(cell_down(0, 2) && !cell_down(0, 3) && !m.key_shift,
              "Left under Games is (0,2), unshifted");

        /* Right is the CTRL line and no cell. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        keymatrix_event(&k, KEY_EV_PRESSED, 0xB7u);
        keymatrix_field(&k, &m);
        bool any = false;
        for (unsigned c = 0; c < ATOM_KEY_COLS; c++) any |= m.key_col[c] != 0;
        CHECK(m.key_ctrl && !any && !m.key_shift, "Right under Games is CTRL alone");
        CHECK((m.ppi.in_b & 0x40u) == 0, "and pulls port B bit 6 low");

        /* ']' is REPT. Moving and firing at once: both lines together,
         * and each lets go on its own release. */
        keymatrix_event(&k, KEY_EV_PRESSED, ']');
        keymatrix_field(&k, &m);
        CHECK(m.key_ctrl && m.key_rept && (m.ppi.in_c & I8255_IN_C_REPT) == 0,
              "Right and ] held: CTRL and REPT together");
        for (int f = 0; f < 10; f++) keymatrix_field(&k, &m);
        keymatrix_event(&k, KEY_EV_RELEASED, 0xB7u);
        keymatrix_field(&k, &m);
        CHECK(!m.key_ctrl && m.key_rept, "Right let go, ] still held");
        keymatrix_event(&k, KEY_EV_RELEASED, ']');
        keymatrix_field(&k, &m);
        CHECK(!m.key_rept && k.n == 0, "both let go");

        /* Shifted, ']' arrives as '}', and is the same key. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        keymatrix_event(&k, KEY_EV_PRESSED, '}');
        keymatrix_field(&k, &m);
        CHECK(m.key_rept && !cell_down(0, 6), "} is fire too");

        /* The host's own Ctrl still works, and a layout's CTRL is OR-ed
         * with it rather than replacing it. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_CTRL);
        keymatrix_event(&k, KEY_EV_PRESSED, 0xB7u);
        keymatrix_event(&k, KEY_EV_RELEASED, PICOCALC_KEY_CTRL);
        keymatrix_field(&k, &m);
        CHECK(m.key_ctrl, "the layout's CTRL holds after the host's Ctrl lets go");

        /* An unmentioned key keeps its standard binding, so RUN types. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        keymatrix_event(&k, KEY_EV_PRESSED, 'r');
        keymatrix_field(&k, &m);
        CHECK(cell_down(5, 9) && !m.key_shift && !m.key_ctrl, "r keeps R");
        fresh();
        keymatrix_set_layout(&k, cursor);
        keymatrix_event(&k, KEY_EV_PRESSED, 0xB6u);
        keymatrix_field(&k, &m);
        CHECK(cell_down(0, 2) && m.key_shift, "Down keeps its standard shifted UPDOWN");

        /* The Alt layer is never overlaid: Alt+M is the menu whatever a
         * layout binds. */
        static keylayout_t greedy;
        unsigned bad = 0;
        const char *g = "m = CTRL\nk = REPT\nc = SPACE\n";
        CHECK(keylayout_parse(&greedy, "GREEDY", g, strlen(g), &bad) == KL_OK,
              "greedy parses");
        fresh();
        keymatrix_set_layout(&k, &greedy);
        keymatrix_event(&k, KEY_EV_PRESSED, PICOCALC_KEY_ALT);
        keymatrix_event(&k, KEY_EV_PRESSED, 'M');
        keymatrix_field(&k, &m);
        CHECK(k.menu_request && !m.key_ctrl, "Alt+M is the menu under any layout");
        fresh();
        keymatrix_set_layout(&k, &greedy);
        keymatrix_event(&k, KEY_EV_PRESSED, 'M');   /* Shift+m */
        keymatrix_field(&k, &m);
        CHECK(m.key_ctrl && !k.menu_request, "a layout binds the key, shifted or not");

        /* A key keeps the binding it went down with: its release undoes
         * that even when the layout changed in between. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        keymatrix_event(&k, KEY_EV_PRESSED, 0xB7u);
        keymatrix_field(&k, &m);
        keymatrix_set_layout(&k, NULL);
        keymatrix_field(&k, &m);
        CHECK(m.key_ctrl && !cell_down(0, 3), "Right held keeps CTRL across a change");
        keymatrix_event(&k, KEY_EV_RELEASED, 0xB7u);
        for (int f = 0; f < 10; f++) keymatrix_field(&k, &m);
        CHECK(!m.key_ctrl && k.n == 0, "and lets go of CTRL, not of <>");
        keymatrix_event(&k, KEY_EV_PRESSED, 0xB7u);
        for (int f = 0; f < 3; f++) keymatrix_field(&k, &m);
        CHECK(cell_down(0, 3) && !m.key_ctrl, "the next press takes the standard map");

        /* The pacing and bounds of the held set hold with a layout. */
        fresh();
        keymatrix_set_layout(&k, cursor);
        keymatrix_event(&k, KEY_EV_PRESSED, ']');
        keymatrix_event(&k, KEY_EV_RELEASED, ']');
        unsigned down = 0;
        for (int f = 0; f < 20; f++) {
            keymatrix_field(&k, &m);
            if (m.key_rept) down++;
        }
        CHECK(down == ATOM_KEY_MIN_FIELDS, "a tap of fire is %u fields of REPT, want %u",
              down, (unsigned)ATOM_KEY_MIN_FIELDS);
        fresh();
        keymatrix_set_layout(&k, cursor);
        for (int rep = 0; rep < 60; rep++) {
            keymatrix_event(&k, KEY_EV_PRESSED, 0xB4u);
            keymatrix_event(&k, KEY_EV_PRESSED, ']');
            keymatrix_event(&k, KEY_EV_RELEASED, 0xB4u);
            keymatrix_event(&k, KEY_EV_PRESSED, 'z');
            keymatrix_event(&k, KEY_EV_RELEASED, ']');
            keymatrix_event(&k, KEY_EV_RELEASED, 'z');
        }
        for (int f = 0; f < 4000 && (k.q_len || k.n); f++) keymatrix_field(&k, &m);
        any = false;
        for (unsigned c = 0; c < ATOM_KEY_COLS; c++) any |= m.key_col[c] != 0;
        CHECK(k.q_len == 0 && k.n == 0 && k.n_open == 0 && !any && !m.key_rept &&
                  !m.key_ctrl,
              "stuck keys under a layout: %u held, %u open", k.n, k.n_open);
    }

    /* ---- game keymaps: the .map parser -------------------------------- */
    {
        static keylayout_t l;
        unsigned line = 99;

        /* The built-in Games, written as a file (§10.5). */
        const char *ex = "# Games: move on the arrows, fire on ]\n"
                         "name  = GAMES\n"
                         "left  = UPDOWN\n"
                         "right = CTRL\n"
                         "]     = REPT\n";
        CHECK(keylayout_parse(&l, "cursor", ex, strlen(ex), &line) == KL_OK && line == 0,
              "the example parses");
        const keylayout_t *b = &keylayout_builtin[0];
        CHECK(strcmp(l.name, b->name) == 0 && l.n == b->n && l.n_tapes == b->n_tapes,
              "the example is the built-in layout");
        CHECK(memcmp(l.bind, b->bind, sizeof(keymap_t) * b->n) == 0, "same bindings");
        CHECK(l.n_tapes == 0 && !keylayout_for_tape(&l, "GAMES") && !keylayout_for_tape(&l, ""),
              "no tapes line, no tape selects it");

        /* A card file may name its own tapes. */
        const char *tp = "left = UPDOWN\ntapes = INVADE, Rocket\n";
        CHECK(keylayout_parse(&l, "mine", tp, strlen(tp), &line) == KL_OK && l.n_tapes == 2,
              "a tapes line parses");
        CHECK(keylayout_for_tape(&l, "INVADE") && keylayout_for_tape(&l, "rocket"),
              "the tapes it names select it, ignoring case");
        CHECK(!keylayout_for_tape(&l, "INVAD") && !keylayout_for_tape(&l, ""),
              "nothing else does");

        /* '#' is a comment, unless it is the key being bound. */
        const char *hash = "# a comment = not a binding\n#=SPACE\n  # indented comment\n";
        CHECK(keylayout_parse(&l, "hash", hash, strlen(hash), &line) == KL_OK && l.n == 1,
              "# binds once, line %u, %u binding(s)", line, l.n);
        CHECK(l.bind[0].code == keymap_picocalc_canonical('#') && l.bind[0].row == 0 &&
                  l.bind[0].col == 9,
              "# = SPACE binds the # key to SPACE");

        /* CRLF, blank lines, no final newline, the file's own name. */
        const char *crlf = "\r\n  A = SPACE\r\n\r\n\tSPACE=z\r\n= = shift\r\n; = :";
        CHECK(keylayout_parse(&l, "fire", crlf, strlen(crlf), &line) == KL_OK,
              "CRLF parses, line %u", line);
        CHECK(strcmp(l.name, "FIRE") == 0 && l.n == 4, "name from the file name, 4 bindings");
        CHECK(l.bind[0].code == 'a' && l.bind[0].row == 0 && l.bind[0].col == 9 &&
                  l.bind[0].flags == 0, "A = SPACE");
        CHECK(l.bind[1].code == ' ' && l.bind[1].row == 5 && l.bind[1].col == 1, "space = Z");
        CHECK(l.bind[2].code == '=' && l.bind[2].flags == (KM_SHIFT | KM_LINE),
              "'=' binds the SHIFT line");
        CHECK(l.bind[3].code == ';' && l.bind[3].row == 2 && l.bind[3].col == 3, "; = :");

        /* Every failure names its line, and nothing is guessed. */
        static const struct { const char *text; keylayout_status_t st; unsigned line; } bad[] = {
            { "left = UPDOWN\nright\n",          KL_SYNTAX,     2 },
            { "left =\n",                        KL_SYNTAX,     1 },
            { "left up = CTRL\n",                KL_SYNTAX,     1 },
            { "\n\nf1 = CTRL\n",                 KL_BAD_KEY,    3 },
            { "left = BREAK\n",                  KL_BAD_TARGET, 1 },
            { "left = CTRL SHIFT\n",             KL_BAD_TARGET, 1 },
            { "left = CTRL\nLEFT = REPT\n",      KL_DUPLICATE,  2 },
            { "a = CTRL\nA = REPT\n",            KL_DUPLICATE,  2 },
            { "name = A NAME FAR TOO LONG\n",    KL_TOO_LONG,   1 },
            { "tapes = A B C D E\n",             KL_TOO_MANY,   1 },
            { "tapes = SEVENTEEN_LETTERS\n",     KL_TOO_LONG,   1 },
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            keylayout_status_t st = keylayout_parse(&l, "x", bad[i].text, strlen(bad[i].text),
                                                    &line);
            CHECK(st == bad[i].st && line == bad[i].line,
                  "\"%s\": got %s at line %u, want %s at line %u", bad[i].text,
                  keylayout_status_str(st), line, keylayout_status_str(bad[i].st),
                  bad[i].line);
        }

        /* One more binding than config.h allows. */
        static char many[1024];
        size_t at = 0;
        for (unsigned i = 0; i <= ATOM_KEYMAP_BINDINGS; i++) {
            at += (size_t)snprintf(many + at, sizeof many - at, "%c = SPACE\n", 'a' + i);
        }
        CHECK(keylayout_parse(&l, "x", many, at, &line) == KL_TOO_MANY &&
                  line == ATOM_KEYMAP_BINDINGS + 1u, "too many bindings");
    }

    TEST_DONE();
}
