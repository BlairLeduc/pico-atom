/* test_settings.c — the defaults, /atom/pico-atom.cfg's parser, and the
 * menu's save that edits it (settings.h, design.md §11.6, §11.7). Needs
 * no ROM.
 */

#include <string.h>

#include "settings.h"
#include "test_util.h"

static settings_status_t parse(settings_t *s, const char *text, unsigned *line) {
    settings_default(s);
    return settings_parse(s, text, strlen(text), line);
}

int main(void) {
    settings_t s, d;
    unsigned line;

    /* ---- the defaults are the machine's and the menu's ---------------- */
    settings_default(&d);
    {
        atom_config_t cfg;
        atom_config_default(&cfg);
        CHECK(memcmp(&d.machine, &cfg, sizeof cfg) == 0, "machine defaults are atom_config_default's");
        CHECK(d.mono && d.border && !d.dark_bg && d.status && d.volume == 8u &&
              d.backlight == 0u && d.turbo, "host defaults");
        CHECK(!d.keys[0] && !d.tape[0] && !d.drive[0][0] && !d.drive[1][0], "nothing inserted");
    }

    /* ---- an empty file, and one of comments, change nothing ----------- */
    CHECK(parse(&s, "", &line) == SET_OK && line == 0, "empty");
    CHECK(memcmp(&s, &d, sizeof s) == 0, "empty changes nothing");
    CHECK(parse(&s, "# nothing\n\n   # indented\r\n", &line) == SET_OK && line == 0, "comments");
    CHECK(memcmp(&s, &d, sizeof s) == 0, "comments change nothing");

    /* ---- every setting ------------------------------------------------ */
    {
        const char *all =
            "# the README's example\r\n"
            "screen    = colour\r\n"
            "BORDER    = Off\r\n"
            "background = Dark\r\n"
            "status    = off\r\n"
            "backlight = 12\r\n"
            "volume    = 0\r\n"
            "keys      = cursor games\r\n"
            "tape      = cchuck.uef\r\n"
            "turbo     = off\r\n"
            "drive0    = games1.dsk\r\n"
            "drive1    = /atom/discs/My Disc.ssd\r\n"
            "upper_ram = off\r\n"
            "dos       = off\r\n";
        CHECK(parse(&s, all, &line) == SET_OK && line == 0, "every setting: line %u", line);
        CHECK(!s.mono && !s.border && s.dark_bg && !s.status && s.backlight == 12u &&
              s.volume == 0u && !s.turbo, "host");
        CHECK(strcmp(s.keys, "CURSOR GAMES") == 0, "keys uppercased, spaces kept: %s", s.keys);
        CHECK(strcmp(s.tape, "cchuck.uef") == 0, "tape: %s", s.tape);
        CHECK(strcmp(s.drive[0], "games1.dsk") == 0, "drive0: %s", s.drive[0]);
        CHECK(strcmp(s.drive[1], "/atom/discs/My Disc.ssd") == 0, "drive1: %s", s.drive[1]);
        CHECK(!s.machine.upper_ram && !s.machine.atomdos, "machine");
        CHECK(s.machine.via_fitted && s.machine.text_space, "the rest of the machine kept");
    }
    CHECK(parse(&s, "screen = mono\nkeys = Standard\n", &line) == SET_OK, "mono, standard");
    CHECK(s.mono && !s.keys[0], "mono, standard");
    CHECK(parse(&s, "screen = color\n", &line) == SET_OK && !s.mono, "American colour");
    CHECK(parse(&s, "tape =\ndrive0 =   \n", &line) == SET_OK && !s.tape[0], "empty means none");

    /* ---- comments after a value, as the README writes them ------------ */
    CHECK(parse(&s, "screen = colour   # or mono\r\n"
                    "volume = 3\t# 0-8\n"
                    "tape   =          # none\n", &line) == SET_OK && line == 0,
          "trailing comments: line %u", line);
    CHECK(!s.mono && s.volume == 3u && !s.tape[0], "trailing comments stripped");
    CHECK(parse(&s, "drive0 = /atom/discs/side#2.ssd # the second\n", &line) == SET_OK &&
          strcmp(s.drive[0], "/atom/discs/side#2.ssd") == 0,
          "a # inside a path is kept: %s", s.drive[0]);
    CHECK(parse(&s, "screen # = mono\n", &line) == SET_SYNTAX && line == 1,
          "a comment before the = leaves no value");

    /* ---- a bad line changes nothing, and the rest still apply --------- */
    {
        static const struct { const char *text; settings_status_t st; } bad[] = {
            { "screen mono\n",        SET_SYNTAX },
            { "= mono\n",             SET_SYNTAX },
            { "screen =\n",           SET_SYNTAX },
            { "colour = mono\n",      SET_UNKNOWN },
            { "screen = green\n",     SET_BAD_VALUE },
            { "border = yes\n",       SET_BAD_VALUE },
            { "background = grey\n",  SET_BAD_VALUE },
            { "volume = 9\n",         SET_BAD_VALUE },
            { "volume = -1\n",        SET_BAD_VALUE },
            { "volume = 99999999999\n", SET_BAD_VALUE },
            { "backlight = 0\n",      SET_BAD_VALUE },
            { "backlight = 16\n",     SET_BAD_VALUE },
            { "field_hz = 60\n",     SET_UNKNOWN },   /* a constant (§16) */
            { "keys = A NAME LONGER THAN 16\n", SET_TOO_LONG },
        };
        for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
            settings_status_t st = parse(&s, bad[i].text, &line);
            CHECK(st == bad[i].st && line == 1, "%s-> %s, line %u", bad[i].text,
                  settings_status_str(st), line);
            CHECK(memcmp(&s, &d, sizeof s) == 0, "%schanged something", bad[i].text);
        }
    }
    {
        const char *mixed = "volume = 3\nvolume = 5\nscreen = sepia\nborder = off\n";
        CHECK(parse(&s, mixed, &line) == SET_DUPLICATE && line == 2, "first problem: line %u", line);
        CHECK(s.volume == 3u && s.mono && !s.border, "good lines apply around the bad ones");
    }
    {
        /* A value refused does not count as given, so a later line may
         * still set it. */
        CHECK(parse(&s, "volume = 11\nvolume = 4\n", &line) == SET_BAD_VALUE && line == 1, "retry");
        CHECK(s.volume == 4u, "the good line applies");
    }
    {
        char text[ATOM_PATH_MAX + 16];
        memcpy(text, "tape = ", 7);
        memset(text + 7, 'a', ATOM_PATH_MAX);
        text[7 + ATOM_PATH_MAX] = 0;
        CHECK(parse(&s, text, &line) == SET_TOO_LONG && !s.tape[0], "path longer than ATOM_PATH_MAX");

        char huge[400];
        memset(huge, 'x', sizeof huge - 1);
        huge[sizeof huge - 1] = 0;
        CHECK(parse(&s, huge, &line) == SET_TOO_LONG && line == 1, "line too long");
    }
    {
        const char nul[] = "screen = colour\0\nborder = off\n";
        settings_default(&s);
        CHECK(settings_parse(&s, nul, sizeof nul - 1, &line) == SET_SYNTAX && line == 1, "NUL");
        CHECK(s.mono && !s.border, "the NUL line is refused, the next applies");
    }

    /* ---- the menu's save edits the file (§11.6) ------------------------- */
    const char *out;
    size_t out_len;
#define REWRITE(text, set) settings_rewrite(text, strlen(text), set, &out, &out_len)
#define IS(want) (out_len == strlen(want) && memcmp(out, want, out_len) == 0)

    /* Nothing to say: an empty file, or none, stays empty. */
    CHECK(REWRITE("", &d) == SET_OK && out_len == 0, "defaults into an empty file: %zu", out_len);

    /* The user's file, CRLF, with comments, a bad line and a stranger. */
    {
        const char *file =
            "# my PicoCalc\r\n"
            "  screen = color      # the colour board\r\n"
            "volume = 11\r\n"
            "tape =          # none\r\n"
            "turbo = off\r\n"
            "frobnicate = yes\r\n"
            "\r\n"
            "keys = cursor games\r\n"
            "drive0 = /atom/discs/games1.dsk # disc\r\n";
        settings_default(&s);
        CHECK(settings_parse(&s, file, strlen(file), &line) == SET_BAD_VALUE && line == 3, "setup");
        /* What the menu changed: mono, volume, a tape in the deck, and a
         * disc taken out of drive 0 and put in drive 1. */
        s.mono = true;
        s.volume = 5u;
        strcpy(s.tape, "NEW01.uef");
        strcpy(s.drive[0], "");
        strcpy(s.drive[1], "/atom/other/side#2.ssd");
        CHECK(REWRITE(file, &s) == SET_OK, "rewrites");
        const char *want =
            "# my PicoCalc\r\n"
            "  screen = mono       # the colour board\r\n"
            "volume = 11\r\n"
            "tape = NEW01.uef # none\r\n"
            "turbo = off\r\n"
            "frobnicate = yes\r\n"
            "\r\n"
            "keys = cursor games\r\n"
            "drive0 =                        # disc\r\n"
            "volume = 5\r\n"
            "drive1 = /atom/other/side#2.ssd\r\n";
        CHECK(IS(want), "the user's file, edited:\n%.*s\nwanted:\n%s", (int)out_len, out, want);

        /* Saving again changes nothing more. */
        static char again[ATOM_SETTINGS_FILE_MAX];
        memcpy(again, out, out_len);
        size_t again_len = out_len;
        CHECK(settings_rewrite(again, again_len, &s, &out, &out_len) == SET_OK &&
              out_len == again_len && memcmp(out, again, out_len) == 0, "a second save is the same");
    }

    /* A value that already says the same stays as the user wrote it,
     * however it is spelt. */
    {
        const char *file = "screen = Color\nkeys = cursor games\ntape = /atom/tapes/CChuck.UEF\n";
        CHECK(parse(&s, file, &line) == SET_OK, "setup");
        strcpy(s.tape, "cchuck.uef");
        CHECK(REWRITE(file, &s) == SET_OK && IS(file), "unchanged: %.*s", (int)out_len, out);
    }

    /* Appended lines: only what differs from the default, with the
     * file's line ending, after a last line that had none. */
    {
        settings_default(&s);
        s.border = false;
        s.status = false;
        s.backlight = 9u;
        strcpy(s.keys, "GAMES");
        CHECK(REWRITE("# mine\r\nturbo = off", &s) == SET_OK &&
              IS("# mine\r\nturbo = off\r\nborder = off\r\nstatus = off\r\nbacklight = 9\r\n"
                 "keys = GAMES\r\n"), "appended: %.*s", (int)out_len, out);
        s.border = true;
        CHECK(REWRITE("border = off\n", &s) == SET_OK && strncmp(out, "border = on\n", 12) == 0,
              "back to the default is written where the file has it");
        s.keys[0] = 0;
        CHECK(REWRITE("keys = games\n", &s) == SET_OK && strstr(out, "keys = standard\n"),
              "the standard map is written by name");
    }

    /* The README's file, three columns: a value put in, one changed and
     * one taken out all leave the comments where they were. */
    {
        const char *file =
            "screen    = mono       # or colour\n"
            "tape      =            # a file in /atom/tapes/\n"
            "drive0    = games1.dsk # a disc image\n"
            "volume\t= 8\t# 0-8\n";
        CHECK(parse(&s, file, &line) == SET_OK, "setup");
        s.mono = false;
        strcpy(s.tape, "TAPE01.uef");
        s.drive[0][0] = 0;
        s.volume = 3u;
        const char *want =
            "screen    = colour     # or colour\n"
            "tape      = TAPE01.uef # a file in /atom/tapes/\n"
            "drive0    =            # a disc image\n"
            "volume\t= 3\t# 0-8\n";
        CHECK(REWRITE(file, &s) == SET_OK && IS(want), "columns kept:\n%.*s\nwanted:\n%s",
              (int)out_len, out, want);
        strcpy(s.tape, "A-MUCH-LONGER-NAME.uef");
        CHECK(REWRITE(file, &s) == SET_OK &&
              strstr(out, "tape      = A-MUCH-LONGER-NAME.uef # a file"),
              "a value too long for the column pushes its comment one space on:\n%.*s",
              (int)out_len, out);
    }

    /* The backlight: left alone at 0, which is the southbridge's own. */
    {
        settings_default(&s);
        CHECK(REWRITE("backlight = 8 # dim\n", &s) == SET_OK && IS("backlight = 8 # dim\n"),
              "a backlight the menu never moved is left");
        s.backlight = 15u;
        CHECK(REWRITE("backlight = 8 # dim\n", &s) == SET_OK && IS("backlight = 15 # dim\n"),
              "one it moved is written");
    }

    /* Refusals leave nothing to write. */
    settings_default(&s);
    s.volume = 2u;
    CHECK(REWRITE("volume = 3\nborder = off\nvolume = 4\n", &s) == SET_DUPLICATE,
          "a key given twice refuses the save");
    CHECK(REWRITE("volume = 11\nvolume = 4\n", &s) == SET_OK &&
          IS("volume = 11\nvolume = 2\n"), "a refused line is not the key's: %.*s", (int)out_len, out);
    {
        static char big[ATOM_SETTINGS_FILE_MAX + 1];
        memset(big, '#', ATOM_SETTINGS_FILE_MAX - 4u);
        big[ATOM_SETTINGS_FILE_MAX - 4u] = '\n';
        big[ATOM_SETTINGS_FILE_MAX - 3u] = 0;
        CHECK(REWRITE(big, &s) == SET_TOO_LONG, "a result too long for the file refuses");
    }
    strcpy(s.tape, "/atom/tapes/a #1.uef");
    CHECK(REWRITE("", &s) == SET_MISMATCH, "a name the file cannot hold does not read back");

    /* What the port saves a path as. */
    {
        char n[ATOM_PATH_MAX];
        settings_card_name(SETTINGS_TAPE_DIR, "/atom/tapes/x.uef", n);
        CHECK(strcmp(n, "x.uef") == 0, "bare: %s", n);
        settings_card_name(SETTINGS_TAPE_DIR, "/atom/tapes/sub/x.uef", n);
        CHECK(strcmp(n, "/atom/tapes/sub/x.uef") == 0, "a subfolder keeps its path: %s", n);
        settings_card_name(SETTINGS_DISC_DIR, "/atom/tapes/x.uef", n);
        CHECK(strcmp(n, "/atom/tapes/x.uef") == 0, "another folder keeps its path: %s", n);
        settings_card_name(SETTINGS_DISC_DIR, "", n);
        CHECK(n[0] == 0, "none is none");
    }

    TEST_DONE();
}
