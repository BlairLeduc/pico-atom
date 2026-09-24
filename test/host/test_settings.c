/* test_settings.c — the defaults and /atom/pico-atom.cfg's parser
 * (settings.h, design.md §11.7). Needs no ROM.
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
        CHECK(!d.mono && !d.border && d.volume == 8u && d.backlight == 0u && d.turbo,
              "host defaults");
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
            "screen    = mono\r\n"
            "BORDER    = On\r\n"
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
        CHECK(s.mono && s.border && s.backlight == 12u && s.volume == 0u && !s.turbo, "host");
        CHECK(strcmp(s.keys, "CURSOR GAMES") == 0, "keys uppercased, spaces kept: %s", s.keys);
        CHECK(strcmp(s.tape, "cchuck.uef") == 0, "tape: %s", s.tape);
        CHECK(strcmp(s.drive[0], "games1.dsk") == 0, "drive0: %s", s.drive[0]);
        CHECK(strcmp(s.drive[1], "/atom/discs/My Disc.ssd") == 0, "drive1: %s", s.drive[1]);
        CHECK(!s.machine.upper_ram && !s.machine.atomdos, "machine");
        CHECK(s.machine.via_fitted && s.machine.text_space, "the rest of the machine kept");
    }
    CHECK(parse(&s, "screen = colour\nkeys = Standard\n", &line) == SET_OK, "colour, standard");
    CHECK(!s.mono && !s.keys[0], "colour, standard");
    CHECK(parse(&s, "screen = color\n", &line) == SET_OK && !s.mono, "American colour");
    CHECK(parse(&s, "tape =\ndrive0 =   \n", &line) == SET_OK && !s.tape[0], "empty means none");

    /* ---- comments after a value, as the README writes them ------------ */
    CHECK(parse(&s, "screen = mono     # or colour\r\n"
                    "volume = 3\t# 0-8\n"
                    "tape   =          # none\n", &line) == SET_OK && line == 0,
          "trailing comments: line %u", line);
    CHECK(s.mono && s.volume == 3u && !s.tape[0], "trailing comments stripped");
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
        const char *mixed = "volume = 3\nvolume = 5\nscreen = sepia\nborder = on\n";
        CHECK(parse(&s, mixed, &line) == SET_DUPLICATE && line == 2, "first problem: line %u", line);
        CHECK(s.volume == 3u && !s.mono && s.border, "good lines apply around the bad ones");
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
        const char nul[] = "screen = mono\0\nborder = on\n";
        settings_default(&s);
        CHECK(settings_parse(&s, nul, sizeof nul - 1, &line) == SET_SYNTAX && line == 1, "NUL");
        CHECK(!s.mono && s.border, "the NUL line is refused, the next applies");
    }

    TEST_DONE();
}
