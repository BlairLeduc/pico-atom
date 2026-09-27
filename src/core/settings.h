/* settings.h — the emulator's settings at power-on, and the card file
 * that changes them, /atom/pico-atom.cfg (design.md §11.7).
 *
 * settings_default() is where every default lives: the machine's own
 * (atom_config_default) and the host's — the screen, the sound, the keys,
 * and what is in the deck and the drives at boot. The file names only
 * what it changes, one `key = value` a line, as a .map file does (§10.5).
 *
 * The parser is here, in the core, so it is tested on the host. What a
 * value names on the card — a layout, a tape, a disc — is the port's to
 * find, and it says so when it cannot.
 */
#ifndef PICO_ATOM_SETTINGS_H
#define PICO_ATOM_SETTINGS_H

#include <stdbool.h>
#include <stddef.h>

#include "atom.h"
#include "config.h"

#define SETTINGS_DRIVES 2u

/* Where a bare name in the file is looked for (§11.7). */
#define SETTINGS_TAPE_DIR "/atom/tapes"
#define SETTINGS_DISC_DIR "/atom/discs"

/* The utility socket's default file, in /atom/roms/ (§11.1, §13.1). */
#define SETTINGS_UTILITY "utility.rom"

/* The host clocks offered (§3.2): the rated 150 MHz, and 300, the only
 * overclock that keeps the panel's SPI at 75 MHz. */
#define SETTINGS_HOST_MHZ      150u
#define SETTINGS_HOST_MHZ_FAST 300u

typedef struct {
    /* The guest (§7.2): RAM at #4000-#7FFF, AtomDOS, the clock (§12.1). */
    atom_config_t machine;

    /* The utility socket at #A000 (§13.1): a file in /atom/roms/, or ""
     * for none, which the file writes as `none`. The default is
     * SETTINGS_UTILITY, and a card without it leaves the socket empty
     * with nothing said. */
    char     utility[ATOM_PATH_MAX];

    bool     mono;          /* the Display page's screen (§8.7)          */
    bool     border;        /* and its border                            */
    bool     dark_bg;       /* text on dark green or orange, not black   */
    bool     status;        /* the status line in the bottom rows (§8.2) */
    bool     perf;          /* the perf line in the top rows (§13.1)     */
    unsigned volume;        /* 0-8, as the menu shows it                 */
    unsigned backlight;     /* 1-15, as the menu shows it; 0 leaves the
                               southbridge's own level alone             */
    bool     turbo;         /* run unpaced while a UEF plays (§11.3)     */

    /* The Pico's clk_sys in MHz, SETTINGS_HOST_MHZ or
     * SETTINGS_HOST_MHZ_FAST (§3.2): read at power-on, before anything
     * is brought up, and nowhere else. A 4 MHz guest needs the fast one;
     * the port runs it at 2 otherwise. */
    unsigned host_mhz;

    /* A layout's name, uppercase; "" is the standard map (§10.5). */
    char     keys[ATOM_KEYMAP_NAME_LEN + 1];

    /* As written in the file, "" for none. The port resolves a bare
     * name against /atom/tapes/ or /atom/discs/. */
    char     tape[ATOM_PATH_MAX];
    char     drive[SETTINGS_DRIVES][ATOM_PATH_MAX];
} settings_t;

void settings_default(settings_t *s);

typedef enum {
    SET_OK = 0,
    SET_SYNTAX,         /* not key = value                  */
    SET_UNKNOWN,        /* no such setting                  */
    SET_BAD_VALUE,      /* not one of the setting's values  */
    SET_DUPLICATE,      /* the setting was given twice      */
    SET_TOO_LONG,       /* a line, a name, a path, or a rewritten file */
    SET_MISMATCH,       /* a rewrite that does not read back */
} settings_status_t;

/* Apply the file's text over *s, which holds the defaults or an earlier
 * file's values. A line that is wrong changes nothing and the lines after
 * it still apply, so one typing mistake does not cost the rest of the
 * file. Returns the first line's status, and its number in *line (0 when
 * every line was good). */
settings_status_t settings_parse(settings_t *s, const char *text, size_t len,
                                 unsigned *line);
const char *settings_status_str(settings_status_t st);

/* The menu's settings written into the file's text (§11.6): screen,
 * border, background, status, perf, backlight, volume, keys, tape,
 * drive0 and drive1, and the Machine page's upper_ram, dos, clock,
 * utility and host_clock (§13.1). turbo is the file's alone. A backlight of 0 is left
 * as the file has it. The text is edited, not regenerated:
 *
 *   - a key the file already gives keeps its line, its place, its
 *     indentation and its comment; only the value changes, and not even
 *     that if the value there already says the same;
 *   - a key the file does not give is appended only if the value differs
 *     from the default, so a default the user never touched keeps
 *     following the firmware's;
 *   - everything else — comments, blank lines, other keys, lines that do
 *     not parse — is copied as it stands;
 *   - the file's own line ending is kept, and appended lines use it.
 *
 * A key given twice is SET_DUPLICATE, and a result longer than
 * ATOM_SETTINGS_FILE_MAX is SET_TOO_LONG. The result is parsed back
 * before it is returned, and must give *s's values again, or it is
 * SET_MISMATCH. On SET_OK, *out is the new text in a static buffer, valid
 * until the next call. No I/O. */
settings_status_t settings_rewrite(const char *text, size_t len, const settings_t *s,
                                   const char **out, size_t *out_len);

/* How the file names what is at `path`: a bare name for a file directly
 * in `dir`, SETTINGS_TAPE_DIR or SETTINGS_DISC_DIR, otherwise the path
 * as it is. "" stays "". */
void settings_card_name(const char *dir, const char *path, char out[ATOM_PATH_MAX]);

#endif /* PICO_ATOM_SETTINGS_H */
