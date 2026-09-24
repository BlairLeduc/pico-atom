/* menu.c — the emulator's menu (menu.h, design.md §13). */

#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "pico/stdlib.h"

#include "discio.h"
#include "display.h"
#include "kbd.h"
#include "keymapio.h"
#include "keymatrix.h"
#include "log.h"
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
    I_RESUME, I_DISCS, I_TAPES, I_SNAPS, I_DISPLAY, I_KEYS, I_VOLUME, I_RESET,
    I_COUNT
};

/* The snapshot page: left and right choose the slot on any row (§11.5). */
enum { S_SLOT, S_SAVE, S_LOAD, S_DELETE, S_COUNT };

/* The display page (§8.7). */
enum { D_COLOUR, D_BORDER, D_BACKLIGHT, D_COUNT };

#define TAPE_ROWS 11

/* The tape page's first rows are the deck's controls; the files follow. */
enum { T_EJECT, T_PLAY, T_REWIND, T_FIRST };

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
    char             status[TEXT_COLS + 1];

    /* The tape page. */
    bool             tapes;
    unsigned         n_tapes;
    int              tape_sel, tape_top;

    /* The snapshot page. */
    bool             snaps;
    int              snap_sel;

    /* The display page. */
    bool             display;
    int              display_sel;

    /* The disc page: row 0 empties the drive, the images follow. */
    bool             discs;
    unsigned         n_discs;
    int              disc_sel, disc_top;
    unsigned         drive;
} s;

static tapeio_entry_t s_list[ATOM_TAPE_LIST_MAX];
static discio_entry_t s_discs[ATOM_DISC_LIST_MAX];

static void say(const char *fmt, const char *arg) {
    snprintf(s.status, sizeof s.status, fmt, arg);
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_main(void) {
    char line[TEXT_COLS + 1];

    for (int i = 0; i < I_COUNT; i++) {
        switch (i) {
        case I_RESUME: snprintf(line, sizeof line, " RESUME"); break;
        case I_SNAPS:  snprintf(line, sizeof line, " SNAPSHOTS..."); break;
        case I_TAPES:  snprintf(line, sizeof line, " TAPES..."); break;
        case I_DISCS:  snprintf(line, sizeof line, " DISCS..."); break;
        case I_KEYS:
            snprintf(line, sizeof line, " KEYS   < %s >",
                     s.set->layout ? s.set->layout->name : "STANDARD");
            break;
        case I_RESET:  snprintf(line, sizeof line, " RESET (BREAK)"); break;
        case I_VOLUME: snprintf(line, sizeof line, " VOLUME < %u >", s.set->volume); break;
        case I_DISPLAY: snprintf(line, sizeof line, " DISPLAY..."); break;
        }
        textpage_line(s.vram, 2 + i, line, i == s.item);
    }

    const char *ins = tapeio_inserted();
    const char *base = strrchr(ins, '/');
    const cassette_t *cas = &s.m->cas;
    char deck[12] = "";
    if (cas->loaded) {
        snprintf(deck, sizeof deck, " %s %u%%", cas->ended ? "END" : cas->playing ? "PLAY" : "STOP",
                 cassette_percent(cas));
    }
    snprintf(line, sizeof line, " TAPE IN: %.*s%s", cas->loaded ? 12 : 21,
             ins[0] ? (base ? base + 1 : ins) : "NONE", deck);
    textpage_line(s.vram, 12, line, false);

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
    textpage_line(s.vram, 13, line, false);
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
                                               : cas->playing ? "STOP" : "PLAY");
        } else if (i == T_REWIND) {
            snprintf(line, sizeof line, " (REWIND)");
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

static void draw_display(void) {
    char line[TEXT_COLS + 1];
    for (int i = 0; i < D_COUNT; i++) {
        switch (i) {
        case D_COLOUR:
            snprintf(line, sizeof line, " SCREEN          < %s >", s.set->mono ? "MONO" : "COLOUR");
            break;
        case D_BORDER:
            snprintf(line, sizeof line, " BORDER          < %s >", s.set->border ? "ON" : "OFF");
            break;
        case D_BACKLIGHT:
            snprintf(line, sizeof line, " BACKLIGHT       < %u >", s.backlight / BKL_STEP);
            break;
        }
        textpage_line(s.vram, 2 + i, line, i == s.display_sel);
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

static void draw(void) {
    textpage_clear(s.vram);
    textpage_line(s.vram, 0, s.tapes ? " PICO-ATOM: TAPES" : s.discs ? " PICO-ATOM: DISCS"
                             : s.snaps ? " PICO-ATOM: SNAPSHOTS"
                             : s.display ? " PICO-ATOM: DISPLAY" : " PICO-ATOM", true);
    if (s.tapes) draw_tapes();
    else if (s.discs) draw_discs();
    else if (s.snaps) draw_snaps();
    else if (s.display) draw_display();
    else draw_main();
    textpage_line(s.vram, 14, s.status, false);
    textpage_line(s.vram, 15, s.tapes ? " ENTER INSERTS  ESC BACK"
                              : s.discs ? " < > DRIVE  ENTER INSERTS  ESC"
                              : s.snaps ? " < > SLOT  ENTER  ESC BACK"
                              : s.display ? " < > CHANGES  ESC BACK"
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
    (void)sb_write(SB_REG_BKL, (uint8_t)v, NULL);
}

/* ---- keys ----------------------------------------------------------------- */

static void key_main(uint8_t c) {
    switch (c) {
    case PC_UP:   s.item = (s.item + I_COUNT - 1) % I_COUNT; break;
    case PC_DOWN: s.item = (s.item + 1) % I_COUNT; break;
    case PC_LEFT:
    case PC_RIGHT: {
        int dir = c == PC_RIGHT ? 1 : -1;
        if (s.item == I_KEYS) {
            cycle_keys(dir);
        } else if (s.item == I_VOLUME) {
            int v = (int)s.set->volume + dir;
            s.set->volume = (unsigned)(v < 0 ? 0 : v > 8 ? 8 : v);
            __dmb();           /* the volume before the request for it */
            s.set->beep++;
        }
        break;
    }
    case PC_ENTER:
        s.status[0] = 0;
        switch (s.item) {
        case I_RESUME: s.done = true; break;
        case I_SNAPS:  s.snaps = true; s.snap_sel = S_SAVE; break;
        case I_TAPES:  open_tapes(); break;
        case I_DISCS:  open_discs(); break;
        case I_DISPLAY: s.display = true; s.display_sel = D_COLOUR; break;
        case I_RESET:  s.m->cpu.reset_pending = true; s.done = true; break;
        }
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
static void key_display(uint8_t c) {
    switch (c) {
    case PC_UP:   s.display_sel = (s.display_sel + D_COUNT - 1) % D_COUNT; break;
    case PC_DOWN: s.display_sel = (s.display_sel + 1) % D_COUNT; break;
    case PC_LEFT:
    case PC_RIGHT:
    case PC_ENTER:
        if (s.display_sel == D_BACKLIGHT) {
            if (c != PC_ENTER) set_backlight(c == PC_RIGHT ? 1 : -1);
            break;
        }
        if (s.display_sel == D_COLOUR) {
            s.set->mono = !s.set->mono;
        } else {
            s.set->border = !s.set->border;
            /* This page is text, and the VDG's text border is black. */
            say(s.set->border ? " GREEN OR BUFF IN GRAPHICS MODES" : "", "");
        }
        display_set_look(s.set->mono, s.set->border);
        break;
    case PC_ESC:
        s.display = false;
        break;
    }
}

/* The deck's controls act on a UEF, whose tape moves in guest time
 * (§11.3); the page stays open so they can be used together. */
static void deck(int what) {
    atom_t *m = s.m;
    if (!m->cas.loaded) { say(" NO UEF IN THE DECK", ""); return; }
    if (what == T_REWIND) {
        atom_cassette_rewind(m);
        say(" REWOUND", "");
    } else if (m->cas.playing) {
        atom_cassette_play(m, false);
        say(" STOPPED", "");
    } else if (m->cas.ended) {
        say(" AT THE END: REWIND FIRST", "");
    } else {
        atom_cassette_play(m, true);
        say(" PLAYING", "");
    }
}

static void key_tapes(uint8_t c) {
    int last = T_FIRST + (int)s.n_tapes - 1;
    switch (c) {
    case PC_UP:   if (s.tape_sel > 0) s.tape_sel--; break;
    case PC_DOWN: if (s.tape_sel < last) s.tape_sel++; break;
    case PC_ENTER:
        if (s.tape_sel == T_EJECT) {
            tapeio_insert(s.m, NULL);
            say(" TAPE EJECTED", "");
        } else if (s.tape_sel == T_PLAY || s.tape_sel == T_REWIND) {
            deck(s.tape_sel);
            return;
        } else {
            const tapeio_entry_t *e = &s_list[s.tape_sel - T_FIRST];
            if (e->uef) { say(" READING %.20s...", e->hdr.name); draw(); }
            const char *err = tapeio_insert(s.m, e->path);
            if (err) { say(" NOT INSERTED: %.16s", err); return; }
            if (e->uef) say(" LOAD\"%.13s\" THEN A KEY", tapeio_first_name());
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
        else if (s.display) key_display(c);
        else key_main(c);
        draw();
    }
}

void menu_run(atom_t *m, menu_settings_t *set, uint8_t *vram) {
    memset(&s, 0, sizeof s);
    s.m = m;
    s.set = set;
    s.vram = vram;
    s.alt = true;      /* it was opened with Alt held */

    int err = storage_mount();
    s.card = err == 0;
    if (!s.card) say(" NO CARD: NO SNAPSHOTS OR TAPES", "");
    refresh_slots();
    rescan_keys();
    /* A layout a tape chose says so (§10.5). */
    if (!s.status[0] && s.set->layout && s.set->keys_tape[0])
        snprintf(s.status, sizeof s.status, " KEYS CHOSEN BY %.16s", s.set->keys_tape);
    /* The settings file's first problem (§11.7), each time it opens. */
    if (!s.status[0] && settingsio_error()[0]) say(" %.30s", settingsio_error());

    uint8_t r[2] = { 0, 0 };
    s.backlight = sb_read(SB_REG_BKL, r) == SB_OK ? r[1] : 0u;
    if (s.backlight < BKL_MIN || s.backlight > BKL_MAX) s.backlight = 128u;

    printf("  menu         : open%s\n", s.card ? "" : " (no card)");
    draw();

    uint32_t last_poll = time_us_32();
    while (!s.done) {
        if (time_us_32() - last_poll >= POLL_US) {
            last_poll = time_us_32();
            (void)kbd_poll();
            keys();
        }
        log_pump();
        sleep_us(500);
    }

    if (s.card) storage_unmount();
    display_invalidate();
    printf("  menu         : closed\n");
}
