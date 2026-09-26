/* test_status.c — the status line's text, and what core 0 hands over
 * for it (status.h, design.md §8.2). Needs no ROM.
 */

#include <string.h>

#include "atom.h"
#include "status.h"
#include "test_util.h"

static char line[ATOM_STATUS_COLS + 1];

static const char *fmt(const atom_status_t *st, const char *tape, const char *d0,
                       const char *d1) {
    const char *drive[ATOM_FDC_DRIVES] = { d0, d1 };
    status_format(st, tape, drive, line);
    return line;
}

/* The line with its padding taken off, for comparing. */
static const char *trimmed(void) {
    static char t[ATOM_STATUS_COLS + 1];
    memcpy(t, line, sizeof t);
    for (int i = ATOM_STATUS_COLS - 1; i >= 0 && t[i] == ' '; i--) t[i] = 0;
    return t;
}

static atom_t m;

int main(void) {
    atom_status_t st;

    /* ---- the text ---------------------------------------------------- */
    memset(&st, 0, sizeof st);
    fmt(&st, "", "", "");
    CHECK(strlen(line) == ATOM_STATUS_COLS && !trimmed()[0], "nothing to say is a blank line");

    st.deck = STATUS_DECK_PLAY;
    st.percent = 42;
    fmt(&st, "/atom/tapes/cchuck.uef", "", "");
    CHECK(strcmp(trimmed(), "PLAY 42% CCHUCK") == 0, "playing: [%s]", line);
    st.turbo10 = 27;
    fmt(&st, "/atom/tapes/cchuck.uef", "", "");
    CHECK(strcmp(trimmed(), "PLAY 42% CCHUCK 2.7X") == 0, "turbo: [%s]", line);

    memset(&st, 0, sizeof st);
    st.deck = STATUS_DECK_END;
    st.percent = 100;
    fmt(&st, "x.uef", "", "");
    CHECK(strcmp(trimmed(), "END X") == 0, "at the end: [%s]", line);
    st.deck = STATUS_DECK_REC;
    st.percent = 3;
    st.errors = 2;
    fmt(&st, "/atom/tapes/TAPE01.uef", "", "");
    CHECK(strcmp(trimmed(), "REC 3% TAPE01 2 BAD") == 0, "recording: [%s]", line);
    st.deck = STATUS_DECK_PROTECTED;
    st.errors = 0;
    fmt(&st, "/atom/tapes/games.uef", "", "");
    CHECK(strcmp(trimmed(), "PROTECTED GAMES") == 0, "refused: [%s]", line);

    /* Drives on the right; a long tape name gives way to them. */
    memset(&st, 0, sizeof st);
    st.heads = 1u;
    fmt(&st, "", "/atom/discs/games1.dsk", "/atom/discs/other.ssd");
    CHECK(strcmp(line + ATOM_STATUS_COLS - 9, "D0 GAMES1") == 0 && line[0] == ' ',
          "drive 0 at the right: [%s]", line);
    st.heads = 2u;
    st.deck = STATUS_DECK_STOP;
    fmt(&st, "/atom/tapes/A VERY LONG TAPE NAME INDEED, LONGER THAN THE LINE.uef",
        "", "/atom/discs/a-long-disc-name.ssd");
    CHECK(strlen(line) == ATOM_STATUS_COLS, "still one line: [%s]", line);
    CHECK(strncmp(line, "STOP 0% A VERY LONG", 19) == 0, "the tape's name shortened: [%s]", line);
    CHECK(strcmp(line + ATOM_STATUS_COLS - 13, "D1 A-LONG-DIS") == 0, "drive 1 kept: [%s]", line);
    CHECK(line[ATOM_STATUS_COLS - 14] == ' ', "with a space between: [%s]", line);
    st.heads = 2u;
    fmt(&st, "", "", "");
    CHECK(strcmp(line + ATOM_STATUS_COLS - 4, "D1 -") == 0, "a loaded head with no disc: [%s]", line);

    /* ---- what core 0 hands over --------------------------------------- */
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&m, &cfg);
    atom_status(&m, &st);
    CHECK(st.deck == STATUS_DECK_EMPTY && st.heads == 0 && st.turbo10 == 0, "an empty machine");

    static uint8_t img[64] = "UEF File!\0\x0a\x00";
    atom_cassette_insert(&m, img, 12);
    atom_status(&m, &st);
    CHECK(st.deck == STATUS_DECK_STOP, "a tape in, stopped: %u", st.deck);
    CHECK(atom_cassette_record(&m, true) == CAS_REC_PROTECTED, "read-only refuses");
    atom_status(&m, &st);
    CHECK(st.deck == STATUS_DECK_PROTECTED, "and says so: %u", st.deck);

    atom_cassette_insert_rw(&m, img, 12, sizeof img);
    atom_status(&m, &st);
    CHECK(st.deck == STATUS_DECK_STOP, "a new insertion forgets the refusal: %u", st.deck);
    CHECK(atom_cassette_record(&m, true) == CAS_REC_OK, "writable records");
    atom_status(&m, &st);
    CHECK(st.deck == STATUS_DECK_REC && st.percent == 12u * 100u / sizeof img,
          "recording, and how full: %u %u%%", st.deck, st.percent);
    atom_cassette_rewind(&m);
    atom_cassette_play(&m, true);
    CHECK(atom_cassette_recording(&m) && !m.cas.playing,
          "rewind and play leave a recording alone");
    atom_cassette_record(&m, false);
    atom_status(&m, &st);
    CHECK(st.deck == STATUS_DECK_END, "stopped at the end of what it wrote: %u", st.deck);

    m.fdc.drive = 1;
    m.fdc.special[I8271_SR_OUTPUT] |= I8271_OUT_LOAD;
    atom_status(&m, &st);
    CHECK(st.heads == 2u, "drive 1's head is loaded: %u", st.heads);

    /* ---- the perf line (§13.1) ---------------------------------------- */
    {
        perf_line_t p = { .busy1000 = 431, .head100 = 231, .present_us = 11500,
                          .dropped = 0, .underruns = 0, .late = 0 };
        status_perf_format(&p, line);
        CHECK(strlen(line) == ATOM_STATUS_COLS, "the perf line fills the width");
        CHECK(strcmp(trimmed(), "C0 43% 2.31X  LCD 11.5MS  DROP 0  UR 0 0") == 0,
              "§13.1's example: '%s'", trimmed());

        /* At 2 MHz the guest may take most of core 0 (§12.1), and a blip
         * in the audio shows until the next boot. */
        p = (perf_line_t){ .busy1000 = 868, .head100 = 115, .present_us = 16749,
                           .dropped = 3, .underruns = 611, .late = 2 };
        status_perf_format(&p, line);
        CHECK(strcmp(trimmed(), "C0 87% 1.15X LCD 16.7MS DROP 3 UR 611 2") == 0,
              "a busy second closes up to fit: '%s'", trimmed());

        /* Figures past their width are clamped, and the line never
         * spills. */
        p = (perf_line_t){ .busy1000 = 5000, .head100 = 0xFFFFFFFFu, .present_us = 0xFFFFFFFFu,
                           .dropped = 0xFFFFFFFFu, .underruns = 0xFFFFFFFFu,
                           .late = 0xFFFFFFFFu };
        status_perf_format(&p, line);
        CHECK(strlen(line) == ATOM_STATUS_COLS && strncmp(line, "C0 100% 999.99X", 15) == 0,
              "clamped: '%s'", line);
    }

    /* ---- PAUSED -------------------------------------------------------- */
    status_paused_format(line);
    CHECK(strlen(line) == ATOM_STATUS_COLS && strncmp(trimmed(), "PAUSED", 6) == 0,
          "the paused line: '%s'", trimmed());

    /* ---- a UEF at 2 MHz (§12.1) -------------------------------------- */
    {
        static atom_t fast;
        atom_config_t cfg;
        atom_config_default(&cfg);
        cfg.clock_mhz = 2;
        atom_init(&fast, &cfg);
        atom_cassette_insert(&fast, img, 12);
        atom_cassette_play(&fast, true);
        atom_status(&fast, &st);
        CHECK(!fast.cas.playing && st.deck == STATUS_DECK_NEEDS_1MHZ,
              "at 2 MHz the deck does not play, and says so: %u", st.deck);
        fmt(&st, "/atom/tapes/cchuck.uef", "", "");
        CHECK(strcmp(trimmed(), "NEEDS 1 MHZ CCHUCK") == 0, "'%s'", trimmed());
        atom_cassette_insert(&fast, img, 12);
        atom_status(&fast, &st);
        CHECK(st.deck == STATUS_DECK_STOP, "a new insertion forgets it: %u", st.deck);
    }

    TEST_DONE();
}
