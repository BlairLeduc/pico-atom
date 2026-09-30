/* menu.c — the emulator's menu (menu.h, design.md §13). */

#include "menu.h"

#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/watchdog.h"
#include "pico/stdlib.h"

#include "discio.h"
#include "display.h"
#include "pico_atom_version.h"
#include "kbd.h"
#include "keymapio.h"
#include "keymatrix.h"
#include "log.h"
#include "portb.h"
#include "settingsio.h"
#include "snapio.h"
#include "southbridge.h"
#include "storage.h"
#include "tapeio.h"
#include "textpage.h"

/* The southbridge resets its own bus after 2.5 s without a transaction
 * (hardware-notes.md §6.1); the menu polls at the same 30 Hz as the
 * live loop. */
#define POLL_US 33333u

/* The MCU refreshes its battery reading every 20 s (hardware-notes.md
 * §6); reading it more often than this would show nothing new. */
#define BAT_POLL_US 5000000u

#define PC_ENTER  0x0Au
#define PC_ESC    0xB1u
#define PC_LEFT   0xB4u
#define PC_UP     0xB5u
#define PC_DOWN   0xB6u
#define PC_RIGHT  0xB7u

/* Backlight register values step by 16 and clamp to 16-240
 * (hardware-notes.md §4.11). */
#define BKL_STEP  16u
#define BKL_MIN   16u
#define BKL_MAX   240u

enum {
    I_TAPES, I_DISCS, I_SNAPS, I_IO, I_MACHINE, I_RESET, I_SAVE, I_ABOUT, I_COUNT
};

_Static_assert(I_MACHINE + 1 == MENU_FKEYS, "F1-F5 are the first five items");

/* Eight items fill rows 2-9, a blank row under the title, then after
 * another blank row the deck on row 11 and the drives on row 12, above
 * the status row (§13.1). */
#define MAIN_TOP 2

/* The snapshot page: left and right choose the slot on any row (§11.5). */
enum { S_SLOT, S_SAVE, S_LOAD, S_DELETE, S_COUNT };

/* The Setup page: the display (§8.7), then the backlight, the
 * volume, the keymap (§10.5) and VIA port B's page (§7.4). */
enum { D_COLOUR, D_BORDER, D_BACKGROUND, D_STATUS, D_PERF, D_BACKLIGHT, D_VOLUME, D_KEYS,
       D_PORT_B, D_COUNT };

/* VIA port B's page (§7.4): on or off on row 2, then PB0-PB7 on rows
 * 4-11, each with a GPIO or none. */
enum { P_ON, P_PB0, P_COUNT = P_PB0 + SETTINGS_PB_BITS };
#define PB_TOP 4

/* The Machine page (§13.1): five settings staged, and the restart that
 * applies them. The host clock is the Pico's (§3.2), and changing it
 * restarts the Pico, not only the Atom. */
enum { M_RAM, M_CLOCK, M_DOS, M_UTILITY, M_HOST, M_APPLY, M_COUNT };

#define TAPE_ROWS 11

/* The tape page's first rows are the deck's controls; the files follow. */
enum { T_EJECT, T_PLAY, T_REWIND, T_RECORD, T_NEW, T_FIRST };

static struct {
    atom_t          *m;
    menu_settings_t *set;
    uint8_t         *vram;
    bool             card;
    bool             alt;
    bool             done;
    int              item;
    unsigned         slot;
    bool             used[SNAPIO_SLOTS];
    unsigned         backlight;
    int              battery;   /* SB_REG_BAT's byte, -1 if unread */
    char             status[TEXT_COLS + 1];
    bool             direct;    /* opened at a page by F1-F5, F10 or Alt+H */

    /* The tape page. */
    bool             tapes;
    unsigned         n_tapes;
    int              tape_sel, tape_top;

    /* The snapshot page. */
    bool             snaps;
    int              snap_sel;

    /* The Setup page, and port B's under it. */
    bool             io;
    int              io_sel;
    bool             portb;
    int              portb_sel;

    /* The Machine page: what is staged, against the running machine.
     * The utility is an index into s_utils, 0 being none. */
    bool             machine;
    int              machine_sel;
    bool             st_upper, st_dos;
    unsigned         st_mhz, st_host;
    unsigned         st_util, n_utils;

    /* The page of keys (MENU_PAGE_HELP). */
    bool             help;

    /* The About page, and the southbridge's version, read as it opens;
     * the die's temperature then, and with the battery after. */
    bool             about;
    int              sb_ver;
    int              temp_c;

    /* The disc page: row 0 empties the drive, the images follow. */
    bool             discs;
    unsigned         n_discs;
    int              disc_sel, disc_top;
    unsigned         drive;
} s;

static tapeio_entry_t s_list[ATOM_TAPE_LIST_MAX];
static discio_entry_t s_discs[ATOM_DISC_LIST_MAX];
/* NONE, the card's utility ROMs, and the running one if the card has
 * lost it (§13.1). */
static char s_utils[ATOM_ROM_LIST_MAX + 2][ROMS_NAME_MAX];

static void say(const char *fmt, const char *arg) {
    snprintf(s.status, sizeof s.status, fmt, arg);
}

/* The deck's state in a word, as the status line has it (§8.2). */
static const char *deck_word(const cassette_t *cas) {
    if (cas->rec.on) return cas->rec.full ? "FULL" : "REC";
    return cas->ended ? "END" : cas->playing ? "PLAY" : "STOP";
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_main(void) {
    char line[TEXT_COLS + 1];

    for (int i = 0; i < I_COUNT; i++) {
        switch (i) {
        case I_SNAPS:  snprintf(line, sizeof line, " SNAPSHOTS..."); break;
        case I_TAPES:  snprintf(line, sizeof line, " TAPES..."); break;
        case I_DISCS:  snprintf(line, sizeof line, " DISCS..."); break;
        case I_RESET:  snprintf(line, sizeof line, " RESET (BREAK)"); break;
        case I_IO:     snprintf(line, sizeof line, " SETUP..."); break;
        case I_MACHINE: snprintf(line, sizeof line, " MACHINE..."); break;
        case I_SAVE:   snprintf(line, sizeof line, " SAVE SETTINGS"); break;
        case I_ABOUT:  snprintf(line, sizeof line, " ABOUT..."); break;
        }
        textpage_line(s.vram, MAIN_TOP + i, line, i == s.item);
    }

    const char *ins = tapeio_inserted();
    const char *base = strrchr(ins, '/');
    const cassette_t *cas = &s.m->cas;
    char deck[12] = "";
    if (cas->loaded) {
        snprintf(deck, sizeof deck, " %s %u%%", deck_word(cas),
                 cas->rec.on ? cassette_room_percent(cas) : cassette_percent(cas));
    }
    snprintf(line, sizeof line, " TAPE IN: %.*s%s", cas->loaded ? 12 : 21,
             ins[0] ? (base ? base + 1 : ins) : "NONE", deck);
    textpage_line(s.vram, MAIN_TOP + I_COUNT + 1, line, false);

    /* Each drive's image, without its directory or extension. */
    char name[ATOM_FDC_DRIVES][12];
    for (unsigned d = 0; d < ATOM_FDC_DRIVES; d++) {
        const char *p = discio_inserted(d);
        const char *b = strrchr(p, '/');
        snprintf(name[d], sizeof name[d], "%s", p[0] ? (b ? b + 1 : p) : "-");
        char *dot = strrchr(name[d], '.');
        if (dot && dot != name[d]) *dot = 0;
    }
    snprintf(line, sizeof line, " DISC 0: %-9.9s 1: %.9s", name[0], name[1]);
    textpage_line(s.vram, MAIN_TOP + I_COUNT + 2, line, false);
}

static void draw_tapes(void) {
    char line[TEXT_COLS + 1];
    const cassette_t *cas = &s.m->cas;
    textpage_line(s.vram, 2, s.n_tapes ? " NAME             LOAD EXEC  LEN"
                                        : " NO TAPES IN /ATOM/TAPES/", false);
    for (int r = 0; r < TAPE_ROWS; r++) {
        int i = s.tape_top + r;
        line[0] = 0;
        if (i == T_EJECT) {
            snprintf(line, sizeof line, " (EJECT)");
        } else if (i == T_PLAY) {
            snprintf(line, sizeof line, " (%s)", !cas->loaded ? "PLAY: NO UEF IN THE DECK"
                                               : cas->playing ? "STOP"
                                               : s.m->cfg.clock_mhz != 1u ? "PLAY: TAPE NEEDS 1 MHZ"
                                               : "PLAY");
        } else if (i == T_REWIND) {
            snprintf(line, sizeof line, " (REWIND)");
        } else if (i == T_RECORD) {
            snprintf(line, sizeof line, " (%s)", !cas->loaded ? "RECORD: NO UEF IN THE DECK"
                                               : cas->rec.on ? "STOP RECORDING"
                                               : !cas->wbuf ? "RECORD: PROTECTED" : "RECORD");
        } else if (i == T_NEW) {
            snprintf(line, sizeof line, " (NEW TAPE)");
        } else if (i < T_FIRST + (int)s.n_tapes) {
            const tapeio_entry_t *e = &s_list[i - T_FIRST];
            bool in = strcmp(e->path, tapeio_inserted()) == 0;
            if (e->uef) {
                snprintf(line, sizeof line, "%c%-16.16s UEF %6lu", in ? '*' : ' ',
                         e->hdr.name, (unsigned long)e->size);
            } else {
                snprintf(line, sizeof line, "%c%-16.16s %04X %04X %4X", in ? '*' : ' ',
                         e->hdr.name[0] ? e->hdr.name : "\"\"", e->hdr.load, e->hdr.exec,
                         e->hdr.len);
            }
        }
        textpage_line(s.vram, 3 + r, line, i == s.tape_sel);
    }
}

static void draw_snaps(void) {
    char line[TEXT_COLS + 1];
    for (int i = 0; i < S_COUNT; i++) {
        switch (i) {
        case S_SLOT:   snprintf(line, sizeof line, " SLOT            < %u >", s.slot + 1u); break;
        case S_SAVE:   snprintf(line, sizeof line, " SAVE"); break;
        case S_LOAD:   snprintf(line, sizeof line, " LOAD"); break;
        case S_DELETE: snprintf(line, sizeof line, " DELETE"); break;
        }
        textpage_line(s.vram, 2 + i, line, i == s.snap_sel);
    }
    /* Every slot's state, the chosen one marked. */
    for (unsigned i = 0; i < SNAPIO_SLOTS; i++) {
        snprintf(line, sizeof line, "%cSLOT %u: %s", i == s.slot ? '*' : ' ', i + 1u,
                 !s.card ? "NO CARD" : s.used[i] ? "SAVED" : "EMPTY");
        textpage_line(s.vram, 3 + S_COUNT + (int)i, line, false);
    }
}

static void draw_io(void) {
    char line[TEXT_COLS + 1];
    for (int i = 0; i < D_COUNT; i++) {
        switch (i) {
        case D_COLOUR:
            snprintf(line, sizeof line, " SCREEN          < %s >", s.set->mono ? "MONO" : "COLOUR");
            break;
        case D_BORDER:
            snprintf(line, sizeof line, " BORDER          < %s >", s.set->border ? "ON" : "OFF");
            break;
        case D_BACKGROUND:
            snprintf(line, sizeof line, " BACKGROUND      < %s >", s.set->dark_bg ? "DARK" : "BLACK");
            break;
        case D_STATUS:
            snprintf(line, sizeof line, " STATUS LINE     < %s >", s.set->status ? "ON" : "OFF");
            break;
        case D_PERF:
            snprintf(line, sizeof line, " PERF LINE       < %s >", s.set->perf ? "ON" : "OFF");
            break;
        case D_BACKLIGHT:
            snprintf(line, sizeof line, " BACKLIGHT       < %u >", s.backlight / BKL_STEP);
            break;
        case D_VOLUME:
            snprintf(line, sizeof line, " VOLUME          < %u >", s.set->volume);
            break;
        case D_KEYS:
            /* A card layout's name is cut to keep the column. */
            snprintf(line, sizeof line, " KEYS            < %.11s >",
                     s.set->layout ? s.set->layout->name : "STANDARD");
            break;
        case D_PORT_B:
            snprintf(line, sizeof line, " VIA PORT B...     %s", s.set->port_b ? "ON" : "OFF");
            break;
        }
        textpage_line(s.vram, 2 + i, line, i == s.io_sel);
    }
}

static void draw_portb(void) {
    char line[TEXT_COLS + 1];
    snprintf(line, sizeof line, " VIA PORT B      < %s >", s.set->port_b ? "ON" : "OFF");
    textpage_line(s.vram, 2, line, s.portb_sel == P_ON);
    for (unsigned i = 0; i < SETTINGS_PB_BITS; i++) {
        uint8_t gp = s.set->pb_gpio[i];
        char pin[8];
        if (gp == SETTINGS_PB_NC) snprintf(pin, sizeof pin, "NC");
        else snprintf(pin, sizeof pin, "GP%u", gp);
        snprintf(line, sizeof line, " PB%u             < %s >", i, pin);
        textpage_line(s.vram, PB_TOP + (int)i, line, s.portb_sel == P_PB0 + (int)i);
    }
}

static void draw_discs(void) {
    char line[TEXT_COLS + 1];
    snprintf(line, sizeof line, " DRIVE < %c >%.20s", (char)('0' + s.drive % 10u),
             s.n_discs ? "" : "  NONE IN /ATOM/DISCS/");
    textpage_line(s.vram, 2, line, false);
    for (int r = 0; r < TAPE_ROWS; r++) {
        int i = s.disc_top + r;
        line[0] = 0;
        if (i == 0) {
            snprintf(line, sizeof line, " (EMPTY THE DRIVE)");
        } else if (i <= (int)s.n_discs) {
            const discio_entry_t *e = &s_discs[i - 1];
            /* Which drive holds it, if either. */
            char in = ' ';
            for (unsigned d = 0; d < ATOM_FDC_DRIVES; d++)
                if (strcmp(e->path, discio_inserted(d)) == 0) in = (char)('0' + d);
            unsigned kib = e->size / 1024u;
            snprintf(line, sizeof line, "%c%-22.22s %3uK%.2s", in, e->name,
                     kib > 999u ? 999u : kib, e->protect ? " P" : "");
        }
        textpage_line(s.vram, 3 + r, line, i == s.disc_sel);
    }
}

/* A file name as the character ROM shows it: upper case, cut short. */
static void upper(char *out, size_t size, const char *in, size_t max) {
    size_t i = 0;
    for (; in[i] && i < max && i + 1 < size; i++)
        out[i] = (char)(in[i] >= 'a' && in[i] <= 'z' ? in[i] - 32 : in[i]);
    out[i] = 0;
}

static const char *staged_utility(void) {
    return s.st_util ? s_utils[s.st_util] : "";
}

/* The Pico's clock as it runs, in MHz. */
static unsigned host_mhz(void) {
    return clock_get_hz(clk_sys) >= SETTINGS_HOST_MHZ_FAST * 1000000u ? SETTINGS_HOST_MHZ_FAST
                                                                       : SETTINGS_HOST_MHZ;
}

/* Does row r's staged value differ from the running machine? */
static bool machine_changed(int r) {
    const atom_config_t *c = &s.m->cfg;
    switch (r) {
    case M_RAM:     return s.st_upper != c->upper_ram;
    case M_CLOCK:   return s.st_mhz != atom_clock_mhz(c);
    case M_DOS:     return s.st_dos != c->atomdos;
    case M_UTILITY: return strcasecmp(staged_utility(), s.set->utility) != 0;
    case M_HOST:    return s.st_host != host_mhz();
    }
    return false;
}

static bool machine_staged(void) {
    for (int r = 0; r < M_APPLY; r++)
        if (machine_changed(r)) return true;
    return false;
}

static void draw_machine(void) {
    char line[TEXT_COLS + 1], name[13];
    for (int i = 0; i < M_COUNT; i++) {
        char mark = machine_changed(i) ? '*' : ' ';
        switch (i) {
        case M_RAM:
            snprintf(line, sizeof line, "%cRAM             < %s >", mark, s.st_upper ? "32K" : "16K");
            break;
        case M_CLOCK:
            snprintf(line, sizeof line, "%cCLOCK           < %u MHZ >", mark, s.st_mhz);
            break;
        case M_DOS:
            snprintf(line, sizeof line, "%cATOMDOS         < %s >", mark, s.st_dos ? "ON" : "OFF");
            break;
        case M_UTILITY:
            upper(name, sizeof name, s.st_util ? s_utils[s.st_util] : "NONE", 12);
            snprintf(line, sizeof line, "%cUTILITY ROM     < %s >", mark, name);
            break;
        case M_HOST:
            snprintf(line, sizeof line, "%cPICO CLOCK      < %u MHZ >", mark, s.st_host);
            break;
        case M_APPLY:
            snprintf(line, sizeof line, " (APPLY AND RESTART)");
            break;
        }
        textpage_line(s.vram, 2 + i, line, i == s.machine_sel);
    }
    /* The VIA is shown and cannot be changed (§7.3). */
    textpage_line(s.vram, 3 + M_COUNT, " - PROGRAM IN MEMORY IS LOST ON", false);
    textpage_line(s.vram, 4 + M_COUNT, "   RESTART.", false);
    /* §3.2: past the RP2350's rating, and what 4 MHz needs. */
    textpage_line(s.vram, 5 + M_COUNT, " - 4 MHZ REQUIRES OVERCLOCKING", false);
    textpage_line(s.vram, 6 + M_COUNT, "   THE PICO'S CLOCK TO 300 MHZ.", false);
}

/* First eight hex digits of a digest. */
static void hex8(char out[9], const uint8_t *d) {
    snprintf(out, 9, "%02X%02X%02X%02X", d[0], d[1], d[2], d[3]);
}

/* A slot's state for the About page (§13.1): OK the image §11.1 records,
 * ?? another image, ANY the utility socket's, OFF hardware not fitted,
 * -- nothing loaded. */
static const char *slot_word(int slot, rom_state_t st) {
    if (slot == ROM_DOS && !s.m->cfg.atomdos) return "OFF";
    switch (st) {
    case ROM_LOADED:       return romset_slots[slot].has_sha1 ? "OK" : "ANY";
    case ROM_UNRECOGNISED: return "??";
    case ROM_SKIPPED:      return "OFF";
    default:               return "--";
    }
}

static void draw_about(void) {
    char line[TEXT_COLS + 1], name[TEXT_COLS + 1];
    const board_info_t *b = s.set->board;
    const roms_report_t *r = s.set->roms;
    const atom_config_t *c = &s.m->cfg;

    upper(name, sizeof name, PICO_ATOM_VERSION, 21);
    snprintf(line, sizeof line, " PICO-ATOM %.21s", name);
    textpage_line(s.vram, 2, line, false);
    /* The die's temperature, whole degrees and uncalibrated
     * (hardware-notes.md §8.1), at the row's right end. */
    upper(name, sizeof name, b->sdk_board, 21);
    snprintf(line, sizeof line, " BOARD %-21.21s", name);
    textpage_line(s.vram, 3, line, false);
    upper(name, sizeof name, b->sdk_platform, 8);
    char sb[4] = "??";
    if (s.sb_ver >= 0) snprintf(sb, sizeof sb, "%02X", (unsigned)(s.sb_ver & 0xFF));
    /* Clamped so the row is at most TEXT_COLS whatever the sensor says. */
    int t = s.temp_c < -99 ? -99 : s.temp_c > 999 ? 999 : s.temp_c;
    snprintf(line, sizeof line, " %-6.6s REV %X %3u MHZ SB %2.2s %3dC", name, b->chip_version & 0xFu,
             (unsigned)((b->clk_sys_hz / 1000000u) % 1000u), sb, t);
    textpage_line(s.vram, 4, line, false);
    snprintf(line, sizeof line, " MACHINE %s %u MHZ%s%s", c->upper_ram ? "32K" : "16K",
             atom_clock_mhz(c), c->atomdos ? " DOS" : "", c->via_fitted ? " VIA" : "");
    textpage_line(s.vram, 5, line, false);

    /* The sockets from the top of the map down, as §11.1 lists them. */
    static const int order[ROM_SLOT_COUNT] = { ROM_KERNEL, ROM_BASIC, ROM_FLOAT, ROM_DOS,
                                               ROM_UTILITY };
    for (int i = 0; i < ROM_SLOT_COUNT; i++) {
        int sl = order[i];
        rom_state_t st = r->slot[sl];
        const char *file = sl == ROM_UTILITY ? (r->utility[0] ? r->utility : "NONE")
                                             : romset_slots[sl].file;
        upper(name, sizeof name, file, 12);
        char h[9] = "";
        if (st == ROM_LOADED || st == ROM_UNRECOGNISED) hex8(h, r->sha1[sl]);
        snprintf(line, sizeof line, " #%04X %-12s %-3s %s", romset_slots[sl].addr, name,
                 slot_word(sl, st), h);
        textpage_line(s.vram, 7 + i, line, false);
    }

    const char *err = settingsio_error();
    snprintf(line, sizeof line, " SETTINGS %.22s", err[0] ? err : "OK");
    textpage_line(s.vram, 8 + ROM_SLOT_COUNT, line, false);
}

/* The keys the emulator takes for itself (§10.3, §13), one a row. */
static void draw_help(void) {
    static const char *const keys[][2] = {
        { "F1",         "TAPES" },
        { "F2",         "DISCS" },
        { "F3",         "SNAPSHOTS" },
        { "F4",         "SETUP" },
        { "F5",         "MACHINE" },
        { "F10",        "ABOUT" },
        { "ALT+M",      "MENU" },
        { "ALT+P",      "PAUSE" },
        { "ALT+K",      "BREAK" },
        { "ALT+C",      "COPY" },
        { "ALT+L",      "LOCK" },
        { "TAB",        "REPT" },
    };
    char line[TEXT_COLS + 1];
    for (unsigned i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        snprintf(line, sizeof line, " %-11s %s", keys[i][0], keys[i][1]);
        textpage_line(s.vram, 2 + (int)i, line, false);
    }
}

/* The title row's right end: the charge, and CHG in place of BAT while
 * it charges (bit 7, hardware-notes.md §6). Nothing if it could not be
 * read. */
static void draw_battery(void) {
    if (s.battery < 0) return;
    unsigned pct = (unsigned)s.battery & 0x7Fu;
    char text[12];
    snprintf(text, sizeof text, "%s %u%% ", s.battery & 0x80 ? "CHG" : "BAT",
             pct > 100u ? 100u : pct);
    textpage_put(s.vram, 0, TEXT_COLS - (int)strlen(text), text, true);
}

static bool read_battery(void) {
    uint8_t r[2];
    int was = s.battery;
    s.battery = sb_read(SB_REG_BAT, r) == SB_OK ? r[1] : -1;
    return s.battery != was;
}

static void draw(void) {
    textpage_clear(s.vram);
    textpage_line(s.vram, 0, s.tapes ? " PICO-ATOM: TAPES" : s.discs ? " PICO-ATOM: DISCS"
                             : s.snaps ? " PICO-ATOM: SNAPSHOTS"
                             : s.portb ? " PICO-ATOM: VIA PORT B"
                             : s.io ? " PICO-ATOM: SETUP"
                             : s.machine ? " PICO-ATOM: MACHINE"
                             : s.about ? " PICO-ATOM: ABOUT"
                             : s.help ? " PICO-ATOM: KEYS" : " PICO-ATOM", true);
    draw_battery();
    if (s.tapes) draw_tapes();
    else if (s.discs) draw_discs();
    else if (s.snaps) draw_snaps();
    else if (s.portb) draw_portb();
    else if (s.io) draw_io();
    else if (s.machine) draw_machine();
    else if (s.about) draw_about();
    else if (s.help) draw_help();
    else draw_main();
    textpage_line(s.vram, 14, s.status, false);
    textpage_line(s.vram, 15, s.tapes ? " ENTER INSERTS  ESC BACK"
                              : s.discs ? " < > DRIVE  ENTER INSERTS  ESC"
                              : s.snaps ? " < > SLOT  ENTER  ESC BACK"
                              : s.portb || s.io ? " < > CHANGES  ESC BACK"
                              : s.machine ? " < > STAGES  ENTER  ESC BACK"
                              : s.about ? " ESC BACK"
                              : s.help ? " ESC RESUMES"
                                        : " ARROWS  ENTER  ESC RESUMES", true);
    display_present(s.vram, 0, NULL);
}

/* ---- actions -------------------------------------------------------------- */

static void refresh_slots(void) {
    for (unsigned i = 0; i < SNAPIO_SLOTS; i++) s.used[i] = s.card && snapio_exists(i);
}

static void do_save(void) {
    if (!s.card) { say(" NO CARD", ""); return; }
    uint32_t t0 = time_us_32();
    snap_status_t st = snapio_save(s.m, s.slot);
    uint32_t ms = (time_us_32() - t0) / 1000u;
    printf("  snapshot     : save slot %u: %s, %lu ms\n", s.slot + 1u,
           snapshot_status_str(st), (unsigned long)ms);
    say(st == SNAP_OK ? " SAVED" : " SAVE FAILED: %s", snapshot_status_str(st));
    refresh_slots();
}

static void do_load(void) {
    if (!s.card) { say(" NO CARD", ""); return; }
    bool recovered = false;
    uint32_t t0 = time_us_32();
    snap_status_t st = snapio_load(s.m, s.slot, &recovered);
    uint32_t ms = (time_us_32() - t0) / 1000u;
    printf("  snapshot     : load slot %u: %s%s, %lu ms\n", s.slot + 1u,
           snapshot_status_str(st), recovered ? " (from the unpublished .new)" : "",
           (unsigned long)ms);
    if (st == SNAP_OK) {
        s.done = true;         /* straight back into the restored machine */
        return;
    }
    if (st == SNAP_OTHER_CLOCK) {
        say(" NOT LOADED: ANOTHER CLOCK", "");   /* §12.1 */
        return;
    }
    say(" NOT LOADED: %s", snapshot_status_str(st));
}

static void do_delete(void) {
    if (!s.card) { say(" NO CARD", ""); return; }
    say(snapio_delete(s.slot) ? " DELETED" : " NOTHING TO DELETE", "");
    refresh_slots();
}

static void open_tapes(void) {
    s.n_tapes = s.card ? tapeio_list(s_list, ATOM_TAPE_LIST_MAX) : 0;
    s.tapes = true;
    s.tape_sel = T_EJECT;
    for (unsigned i = 0; i < s.n_tapes; i++) {
        if (strcmp(s_list[i].path, tapeio_inserted()) == 0) s.tape_sel = T_FIRST + (int)i;
    }
    s.tape_top = s.tape_sel >= TAPE_ROWS ? s.tape_sel - TAPE_ROWS + 1 : 0;
    s.status[0] = 0;
}

static void open_discs(void) {
    s.n_discs = s.card ? discio_list(s_discs, ATOM_DISC_LIST_MAX) : 0;
    s.discs = true;
    s.disc_sel = 0;
    for (unsigned i = 0; i < s.n_discs; i++) {
        if (strcmp(s_discs[i].path, discio_inserted(s.drive)) == 0) s.disc_sel = (int)i + 1;
    }
    s.disc_top = s.disc_sel >= TAPE_ROWS ? s.disc_sel - TAPE_ROWS + 1 : 0;
    s.status[0] = 0;
}

/* Standard, then keymapio's list, round and round. */
static void cycle_keys(int dir) {
    int n = (int)keymapio_count() + 1;
    int at = s.set->layout ? keymapio_find(s.set->layout->name) + 1 : 0;
    at = (at + n + dir) % n;
    s.set->layout = at ? keymapio_get((unsigned)(at - 1)) : NULL;
    s.set->keys_tape[0] = 0;   /* the user's choice now */
}

/* The card's layouts are read afresh, which rewrites the one in force if
 * it came from the card: find it again by name, or fall back to the
 * standard map if its file has gone. */
static void rescan_keys(void) {
    char name[ATOM_KEYMAP_NAME_LEN + 1] = "";
    if (s.set->layout) memcpy(name, s.set->layout->name, sizeof name);
    if (!s.card) return;
    keymapio_scan();
    int i = name[0] ? keymapio_find(name) : -1;
    s.set->layout = i >= 0 ? keymapio_get((unsigned)i) : NULL;
    if (!s.set->layout) s.set->keys_tape[0] = 0;
    if (keymapio_error()[0]) say(" %.30s", keymapio_error());
}

static void set_backlight(int dir) {
    int v = (int)s.backlight + dir * (int)BKL_STEP;
    if (v < (int)BKL_MIN) v = BKL_MIN;
    if (v > (int)BKL_MAX) v = BKL_MAX;
    s.backlight = (unsigned)v;
    s.set->backlight = s.backlight / BKL_STEP;
    (void)sb_write(SB_REG_BKL, (uint8_t)v, NULL);
}

/* The menu's settings into the settings file (§11.6). What the menu
 * does not set is saved as the file had it; so are the keys while a
 * tape's load chose them, since the user did not. A tape or disc in its
 * own folder is saved by its bare name. */
static void save_settings(void) {
    if (!s.card) { say(" NO CARD", ""); return; }
    settings_t out = *s.set->file;
    out.mono = s.set->mono;
    out.border = s.set->border;
    out.dark_bg = s.set->dark_bg;
    out.status = s.set->status;
    out.volume = s.set->volume;
    out.backlight = s.set->backlight;
    out.perf = s.set->perf;
    out.port_b = s.set->port_b;
    memcpy(out.pb_gpio, s.set->pb_gpio, sizeof out.pb_gpio);
    /* The machine as it is running, not as the Machine page has it
     * staged (§11.6, §13.1). */
    out.machine.upper_ram = s.m->cfg.upper_ram;
    out.machine.atomdos = s.m->cfg.atomdos;
    out.machine.clock_mhz = s.m->cfg.clock_mhz;
    snprintf(out.utility, sizeof out.utility, "%s", s.set->utility);
    if (!s.set->keys_tape[0])
        snprintf(out.keys, sizeof out.keys, "%s", s.set->layout ? s.set->layout->name : "");
    settings_card_name(SETTINGS_TAPE_DIR, tapeio_inserted(), out.tape);
    for (unsigned d = 0; d < SETTINGS_DRIVES; d++)
        settings_card_name(SETTINGS_DISC_DIR, discio_inserted(d), out.drive[d]);
    const char *err = settingsio_save(&out);
    if (!err) *s.set->file = out;
    say(err ? " NOT SAVED: %.20s" : " SETTINGS SAVED", err);
}

/* The Machine page stages the running machine (§13.1). */
static void open_machine(void) {
    const atom_config_t *c = &s.m->cfg;
    s.machine = true;
    s.machine_sel = M_RAM;
    s.st_upper = c->upper_ram;
    s.st_dos = c->atomdos;
    s.st_mhz = atom_clock_mhz(c);
    s.st_host = host_mhz();

    s_utils[0][0] = 0;
    s.n_utils = 1u + (s.card ? roms_list_utility(&s_utils[1], ATOM_ROM_LIST_MAX) : 0u);
    s.st_util = 0;
    for (unsigned i = 1; i < s.n_utils; i++)
        if (strcasecmp(s_utils[i], s.set->utility) == 0) s.st_util = i;
    /* One the card no longer has stays the staged value, so that
     * nothing reads as changed that the user did not change. */
    if (!s.st_util && s.set->utility[0]) {
        snprintf(s_utils[s.n_utils], ROMS_NAME_MAX, "%s", s.set->utility);
        s.st_util = s.n_utils++;
    }
    s.status[0] = 0;
}

static void open_about(void) {
    uint8_t r[2];
    s.about = true;
    s.sb_ver = sb_read(SB_REG_VER, r) == SB_OK ? r[1] : -1;
    s.temp_c = board_temp_c();
    s.status[0] = 0;
}

/* Apply and restart (§13.1): a power-on of the staged machine, or the
 * running one left as it is, and the status row says why. */
static void apply_machine(void) {
    atom_t *m = s.m;
    if (!s.card) { say(" NO CARD: NOT RESTARTED", ""); return; }
    /* The recording is not on the card until it stops (§11.3). */
    if (atom_cassette_recording(m)) { say(" STOP RECORDING FIRST", ""); return; }
    if (m->cas.dirty) { say(" RECORDING NOT ON THE CARD YET", ""); return; }
    atom_config_t cfg = m->cfg;
    cfg.upper_ram = s.st_upper;
    cfg.atomdos = s.st_dos;
    cfg.clock_mhz = (uint8_t)s.st_mhz;
    if (s.st_host != host_mhz()) {
        /* The clock is set before anything is brought up, and only at
         * power-on (§3.2): the staged machine goes into the settings
         * file with it, since the file is all a restart of the Pico
         * reads (§11.6), and the Pico restarts. */
        settings_t out = *s.set->file;
        out.machine.upper_ram = cfg.upper_ram;
        out.machine.atomdos = cfg.atomdos;
        out.machine.clock_mhz = cfg.clock_mhz;
        snprintf(out.utility, sizeof out.utility, "%s", staged_utility());
        out.host_mhz = s.st_host;
        const char *err = settingsio_save(&out);
        if (err) { say(" NOT SAVED: %.20s", err); return; }
        *s.set->file = out;
        printf("  machine      : host clock %u MHz saved; restarting the Pico\n", s.st_host);
        say(" RESTARTING THE PICO...", "");
        draw();
        storage_unmount();
        busy_wait_ms(100);   /* the UART's last line */
        watchdog_reboot(0, 0, 0);
        for (;;) tight_loop_contents();
    }
    say(" RESTARTING...", "");
    draw();
    const char *err = s.set->restart(&cfg, staged_utility());
    if (err) { say(" %.30s", err); return; }
    s.done = true;             /* straight into the new machine */
}

static void key_machine(uint8_t c) {
    switch (c) {
    case PC_UP:   s.machine_sel = (s.machine_sel + M_COUNT - 1) % M_COUNT; break;
    case PC_DOWN: s.machine_sel = (s.machine_sel + 1) % M_COUNT; break;
    case PC_LEFT:
    case PC_RIGHT: {
        int dir = c == PC_RIGHT ? 1 : -1;
        switch (s.machine_sel) {
        case M_RAM:     s.st_upper = !s.st_upper; break;
        case M_CLOCK: {
            /* 1, 2, and 4 on a host at 300 (§12.1). */
            static const unsigned clocks[] = { 1u, 2u, 4u };
            unsigned n = s.st_host == SETTINGS_HOST_MHZ_FAST ? 3u : 2u, i = 0;
            while (i < n && clocks[i] != s.st_mhz) i++;
            s.st_mhz = clocks[(i + n + (unsigned)dir) % n];
            break;
        }
        case M_HOST:
            s.st_host = s.st_host == SETTINGS_HOST_MHZ ? SETTINGS_HOST_MHZ_FAST : SETTINGS_HOST_MHZ;
            if (s.st_host == SETTINGS_HOST_MHZ && s.st_mhz == 4u) s.st_mhz = 2u;
            break;
        case M_DOS:     s.st_dos = !s.st_dos; break;
        case M_UTILITY: s.st_util = (s.st_util + s.n_utils + (unsigned)dir) % s.n_utils; break;
        }
        say(!machine_staged() ? ""
            : s.st_host != host_mhz() ? " APPLY SAVES, RESTARTS THE PICO"
            : " APPLY RESTARTS: PROGRAM LOST", "");
        break;
    }
    case PC_ENTER:
        if (s.machine_sel == M_APPLY) {
            if (machine_staged()) apply_machine();
            else say(" NOTHING TO APPLY", "");
        }
        break;
    case PC_ESC:
        s.machine = false;
        /* Nothing changes until Apply (§13.1). */
        say(machine_staged() ? " NOT APPLIED" : "", "");
        break;
    }
}

static void key_about(uint8_t c) {
    if (c == PC_ESC || c == PC_ENTER) s.about = false;
}

static void key_help(uint8_t c) {
    if (c == PC_ESC || c == PC_ENTER) s.help = false;
}

/* ---- keys ----------------------------------------------------------------- */

static void open_item(void) {
    s.status[0] = 0;
    switch (s.item) {
    case I_SNAPS:  s.snaps = true; s.snap_sel = S_SAVE; break;
    case I_TAPES:  open_tapes(); break;
    case I_DISCS:  open_discs(); break;
    case I_IO:     s.io = true; s.io_sel = D_COLOUR; break;
    case I_MACHINE: open_machine(); break;
    case I_ABOUT:  open_about(); break;
    case I_RESET:  s.m->cpu.reset_pending = true; s.done = true; break;
    case I_SAVE:   save_settings(); break;
    }
}

static void key_main(uint8_t c) {
    switch (c) {
    case PC_UP:   s.item = (s.item + I_COUNT - 1) % I_COUNT; break;
    case PC_DOWN: s.item = (s.item + 1) % I_COUNT; break;
    case PC_ENTER:
        open_item();
        break;
    case PC_ESC:
        s.done = true;
        break;
    }
}

static void key_snaps(uint8_t c) {
    switch (c) {
    case PC_UP:   s.snap_sel = (s.snap_sel + S_COUNT - 1) % S_COUNT; break;
    case PC_DOWN: s.snap_sel = (s.snap_sel + 1) % S_COUNT; break;
    case PC_LEFT:
    case PC_RIGHT:
        s.slot = (s.slot + (c == PC_RIGHT ? 1u : SNAPIO_SLOTS - 1u)) % SNAPIO_SLOTS;
        break;
    case PC_ENTER:
        s.status[0] = 0;
        switch (s.snap_sel) {
        case S_SAVE:   say(" SAVING...", ""); draw(); do_save(); break;
        case S_LOAD:   say(" LOADING...", ""); draw(); do_load(); break;
        case S_DELETE: do_delete(); break;
        }
        break;
    case PC_ESC:
        s.snaps = false;
        break;
    }
}

/* Each change shows at once: the page itself is drawn through the
 * renderer it changes. */
static void key_io(uint8_t c) {
    switch (c) {
    case PC_UP:   s.io_sel = (s.io_sel + D_COUNT - 1) % D_COUNT; break;
    case PC_DOWN: s.io_sel = (s.io_sel + 1) % D_COUNT; break;
    case PC_LEFT:
    case PC_RIGHT:
    case PC_ENTER:
        if (s.io_sel == D_PORT_B) {
            if (c == PC_ENTER) { s.portb = true; s.portb_sel = P_ON; s.status[0] = 0; }
            break;
        }
        if (s.io_sel == D_BACKLIGHT) {
            if (c != PC_ENTER) set_backlight(c == PC_RIGHT ? 1 : -1);
            break;
        }
        if (s.io_sel == D_VOLUME || s.io_sel == D_KEYS) {
            if (c == PC_ENTER) break;
            int dir = c == PC_RIGHT ? 1 : -1;
            if (s.io_sel == D_KEYS) {
                cycle_keys(dir);
            } else {
                int v = (int)s.set->volume + dir;
                s.set->volume = (unsigned)(v < 0 ? 0 : v > 8 ? 8 : v);
                __dmb();           /* the volume before the request for it */
                s.set->beep++;
            }
            break;
        }
        if (s.io_sel == D_STATUS || s.io_sel == D_PERF) {
            /* Core 1 draws the lines; they show once the menu closes. */
            if (s.io_sel == D_STATUS) s.set->status = !s.set->status;
            else s.set->perf = !s.set->perf;
            break;
        }
        if (s.io_sel == D_COLOUR) {
            s.set->mono = !s.set->mono;
        } else if (s.io_sel == D_BACKGROUND) {
            s.set->dark_bg = !s.set->dark_bg;
        } else {
            s.set->border = !s.set->border;
            /* This page is text, and the VDG's text border is black. */
            say(s.set->border ? " GREEN OR BUFF IN GRAPHICS MODES" : "", "");
        }
        display_set_look(s.set->mono, s.set->border, s.set->dark_bg);
        break;
    case PC_ESC:
        s.io = false;
        break;
    }
}

/* Port B's pins as the page has them, applied at once (§7.4); the
 * status row says what the UART has lost to them: the log with TX, the
 * typed keys with RX. */
static void apply_portb(void) {
    portb_set(s.m, s.set->port_b, s.set->pb_gpio);
    bool tx = portb_has_uart_tx(), rx = portb_has_uart_rx();
    say(tx && rx ? " GP4/GP5: NO UART LOG OR KEYS"
        : tx     ? " GP4: NO UART LOG"
        : rx     ? " GP5: NO UART KEYS"
                 : "", "");
}

/* The next GPIO for PB`bit` in direction `dir`: NC, then the free pins
 * in order (settings.h), passing any another bit holds. */
static uint8_t next_gpio(unsigned bit, int dir) {
    uint8_t list[33];
    unsigned n = 0, at = 0;
    list[n++] = SETTINGS_PB_NC;
    for (unsigned gp = 0; gp < 32u; gp++)
        if (SETTINGS_PB_GPIOS & (1u << gp)) list[n++] = (uint8_t)gp;
    for (unsigned i = 0; i < n; i++)
        if (list[i] == s.set->pb_gpio[bit]) at = i;
    for (unsigned step = 0; step < n; step++) {
        at = (at + (dir > 0 ? 1u : n - 1u)) % n;
        bool taken = false;
        for (unsigned i = 0; i < SETTINGS_PB_BITS; i++)
            if (i != bit && list[at] != SETTINGS_PB_NC && s.set->pb_gpio[i] == list[at]) taken = true;
        if (!taken) break;
    }
    return list[at];
}

static void key_portb(uint8_t c) {
    switch (c) {
    case PC_UP:   s.portb_sel = (s.portb_sel + P_COUNT - 1) % P_COUNT; break;
    case PC_DOWN: s.portb_sel = (s.portb_sel + 1) % P_COUNT; break;
    case PC_LEFT:
    case PC_RIGHT:
    case PC_ENTER:
        if (s.portb_sel == P_ON) {
            s.set->port_b = !s.set->port_b;
        } else {
            if (c == PC_ENTER) break;
            unsigned bit = (unsigned)(s.portb_sel - P_PB0);
            s.set->pb_gpio[bit] = next_gpio(bit, c == PC_RIGHT ? 1 : -1);
        }
        apply_portb();
        break;
    case PC_ESC:
        s.portb = false;
        s.status[0] = 0;
        break;
    }
}

/* The deck's controls act on a UEF, whose tape moves in guest time
 * (§11.3); the page stays open so they can be used together. */
static void deck(int what) {
    atom_t *m = s.m;
    if (!m->cas.loaded) { say(" NO UEF IN THE DECK", ""); return; }
    if (atom_cassette_recording(m)) { say(" RECORDING: STOP IT FIRST", ""); return; }
    if (what == T_REWIND) {
        atom_cassette_rewind(m);
        say(" REWOUND", "");
    } else if (m->cas.playing) {
        atom_cassette_play(m, false);
        say(" STOPPED", "");
    } else if (m->cas.ended) {
        say(" AT THE END: REWIND FIRST", "");
    } else if (m->cfg.clock_mhz != 1u) {
        /* The MOS reads a tape at 1 MHz only (§12.1, §16). */
        say(" TAPE NEEDS 1 MHZ", "");
    } else {
        atom_cassette_play(m, true);
        say(" PLAYING", "");
    }
}

/* Recording by hand (§11.3), for a saver that never calls OSSAVE and so
 * gives no cue. Stopping writes the tape to the card at once. */
static void record(void) {
    atom_t *m = s.m;
    if (!m->cas.loaded) { say(" NO UEF IN THE DECK", ""); return; }
    if (atom_cassette_recording(m)) {
        (void)atom_cassette_record(m, false);
        const char *err = m->cas.dirty ? tapeio_write(m) : NULL;
        if (err) { say(" NOT WRITTEN: %.18s", err); return; }
        snprintf(s.status, sizeof s.status, " RECORDED %lu BYTES",
                 (unsigned long)m->cas.rec.bytes);
        return;
    }
    switch (atom_cassette_record(m, true)) {
    case CAS_REC_OK:        say(" RECORDING: SAVE, THEN STOP", ""); break;
    case CAS_REC_PROTECTED: say(" PROTECTED: NOT RECORDING", ""); break;
    case CAS_REC_FULL:      say(" TAPE FULL", ""); break;
    default:                say(" NO UEF IN THE DECK", ""); break;
    }
}

static void new_tape(void) {
    if (!s.card) { say(" NO CARD", ""); return; }
    const char *err = tapeio_new(s.m);
    if (err) { say(" NO NEW TAPE: %.18s", err); return; }
    const char *b = strrchr(tapeio_inserted(), '/');
    say(" IN: %.12s, SAVE ONTO IT", b ? b + 1 : tapeio_inserted());
}

static void key_tapes(uint8_t c) {
    int last = T_FIRST + (int)s.n_tapes - 1;
    switch (c) {
    case PC_UP:   if (s.tape_sel > 0) s.tape_sel--; break;
    case PC_DOWN: if (s.tape_sel < last) s.tape_sel++; break;
    case PC_ENTER:
        if (s.tape_sel == T_EJECT) {
            const char *err = tapeio_insert(s.m, NULL);
            if (err) { say(" NOT WRITTEN: %.18s", err); return; }
            say(" TAPE EJECTED", "");
        } else if (s.tape_sel == T_PLAY || s.tape_sel == T_REWIND) {
            deck(s.tape_sel);
            return;
        } else if (s.tape_sel == T_RECORD) {
            record();
            return;
        } else if (s.tape_sel == T_NEW) {
            new_tape();
            if (s.card) s.n_tapes = tapeio_list(s_list, ATOM_TAPE_LIST_MAX);
            return;
        } else {
            const tapeio_entry_t *e = &s_list[s.tape_sel - T_FIRST];
            if (e->uef) { say(" READING %.20s...", e->hdr.name); draw(); }
            const char *err = tapeio_insert(s.m, e->path);
            if (err) { say(" NOT INSERTED: %.16s", err); return; }
            if (e->uef && s.m->cfg.clock_mhz != 1u) say(" IN, BUT TAPE NEEDS 1 MHZ", "");
            else if (e->uef) say(" LOAD\"%.13s\" THEN A KEY", tapeio_first_name());
            else say(" IN: LOAD\"\" TAKES %.10s", e->hdr.name);
        }
        s.tapes = false;
        return;
    case PC_ESC:
        s.tapes = false;
        return;
    }
    if (s.tape_sel < s.tape_top) s.tape_top = s.tape_sel;
    if (s.tape_sel >= s.tape_top + TAPE_ROWS) s.tape_top = s.tape_sel - TAPE_ROWS + 1;
}

static void key_discs(uint8_t c) {
    switch (c) {
    case PC_UP:   if (s.disc_sel > 0) s.disc_sel--; break;
    case PC_DOWN: if (s.disc_sel < (int)s.n_discs) s.disc_sel++; break;
    case PC_LEFT:
    case PC_RIGHT:
        s.drive = (s.drive + 1u) % ATOM_FDC_DRIVES;
        break;
    case PC_ENTER: {
        if (s.disc_sel == 0) {
            discio_insert(s.m, s.drive, NULL);
            snprintf(s.status, sizeof s.status, " DRIVE %u EMPTIED", s.drive);
        } else {
            const discio_entry_t *e = &s_discs[s.disc_sel - 1];
            /* One image in one drive: two would write over each other. */
            unsigned other = (s.drive + 1u) % ATOM_FDC_DRIVES;
            if (strcmp(e->path, discio_inserted(other)) == 0) discio_insert(s.m, other, NULL);
            const char *err = discio_insert(s.m, s.drive, e->path);
            if (err) { say(" NOT INSERTED: %.16s", err); return; }
            snprintf(s.status, sizeof s.status, " DRIVE %u: *DOS, THEN *CAT", s.drive);
        }
        s.discs = false;
        return;
    }
    case PC_ESC:
        s.discs = false;
        return;
    }
    if (s.disc_sel < s.disc_top) s.disc_top = s.disc_sel;
    if (s.disc_sel >= s.disc_top + TAPE_ROWS) s.disc_top = s.disc_sel - TAPE_ROWS + 1;
}

/* Presses only: releases and the MCU's held reports move nothing. Alt is
 * tracked so that Alt+M closes the menu as it opened it. */
static void keys(void) {
    uint8_t st, c;
    while (!s.done && kbd_pop(&st, &c)) {
        if (c == PICOCALC_KEY_ALT) { s.alt = st != KEY_EV_RELEASED; continue; }
        if (st != KEY_EV_PRESSED) continue;
        if (s.alt && (c == 'm' || c == 'M')) { s.done = true; break; }
        if (s.tapes) key_tapes(c);
        else if (s.discs) key_discs(c);
        else if (s.snaps) key_snaps(c);
        else if (s.portb) key_portb(c);
        else if (s.io) key_io(c);
        else if (s.machine) key_machine(c);
        else if (s.about) key_about(c);
        else if (s.help) key_help(c);
        else key_main(c);
        /* A page F1-F5 opened goes back to the Atom, not the main page. */
        if (s.direct && !(s.tapes || s.discs || s.snaps || s.portb || s.io ||
                          s.machine || s.about || s.help)) {
            s.done = true;
            break;
        }
        draw();
    }
}

void menu_run(atom_t *m, menu_settings_t *set, uint8_t *vram, unsigned page, bool alt) {
    memset(&s, 0, sizeof s);
    s.m = m;
    s.set = set;
    s.vram = vram;
    s.alt = alt;       /* Alt+M or Alt+H has it held; F1-F5 and F10 do not */

    int err = storage_mount();
    s.card = err == 0;
    if (!s.card) say(" NO CARD: NO SNAPSHOTS OR TAPES", "");
    refresh_slots();
    rescan_keys();
    /* A layout a tape chose says so (§10.5). */
    if (!s.status[0] && s.set->layout && s.set->keys_tape[0])
        snprintf(s.status, sizeof s.status, " KEYS CHOSEN BY %.16s", s.set->keys_tape);
    /* The recorder's last word (§11.3), then the settings file's first
     * problem (§11.7), each time it opens. */
    const cassette_t *cas = &m->cas;
    if (!s.status[0] && cas->loaded) {
        if (cas->refused == CAS_REC_PROTECTED) say(" TAPE PROTECTED: NOT RECORDED", "");
        else if (cas->rec.full) say(" TAPE FULL: RECORDING STOPPED", "");
        else if (cas->dirty) say(" RECORDING NOT ON THE CARD YET", "");
        else if (cas->rec.errors)
            snprintf(s.status, sizeof s.status, " %lu BYTES DID NOT FRAME",
                     (unsigned long)cas->rec.errors);
    }
    if (!s.status[0] && settingsio_error()[0]) say(" %.30s", settingsio_error());

    /* Opened by F1-F5, F10 or Alt+H: that page, keeping what the status
     * row says, and closing it closes the menu. */
    if (page == MENU_PAGE_HELP) {
        s.direct = true;
        s.help = true;
    } else if (page == MENU_PAGE_ABOUT) {
        s.direct = true;
        open_about();
    } else if (page >= 1u && page <= MENU_FKEYS) {
        s.direct = true;
        char said[sizeof s.status];
        memcpy(said, s.status, sizeof said);
        s.item = (int)page - 1;
        open_item();
        if (!s.status[0]) memcpy(s.status, said, sizeof said);
    }

    uint8_t r[2] = { 0, 0 };
    s.backlight = sb_read(SB_REG_BKL, r) == SB_OK ? r[1] : 0u;
    if (s.backlight < BKL_MIN || s.backlight > BKL_MAX) s.backlight = 128u;
    s.battery = -1;
    (void)read_battery();

    /* The status line describes the running machine, and the menu can
     * change what it says (§8.2), so it is hidden while the menu is open.
     * It is drawn only when its text changes, so the first present after
     * the menu closes draws it afresh. */
    char blank[ATOM_STATUS_COLS + 1];
    memset(blank, ' ', ATOM_STATUS_COLS);
    blank[ATOM_STATUS_COLS] = 0;
    display_status(blank);
    display_perf(blank);    /* the perf line too (§13.1) */

    printf("  menu         : open%s\n", s.card ? "" : " (no card)");
    draw();

    uint32_t last_poll = time_us_32();
    uint32_t last_bat = last_poll;
    while (!s.done) {
        if (time_us_32() - last_poll >= POLL_US) {
            last_poll = time_us_32();
            (void)kbd_poll();
            keys();
            if (!s.done && last_poll - last_bat >= BAT_POLL_US) {
                last_bat = last_poll;
                bool changed = read_battery();
                if (s.about) {
                    int t = board_temp_c();
                    changed |= t != s.temp_c;
                    s.temp_c = t;
                }
                if (changed) draw();
            }
        }
        log_pump();
        sleep_us(500);
    }

    if (s.card) storage_unmount();
    display_invalidate();
    printf("  menu         : closed\n");
}
