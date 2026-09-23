/* menu.c — the emulator's menu (menu.h, design.md §13). */

#include "menu.h"

#include <stdio.h>
#include <string.h>

#include "pico/stdlib.h"

#include "display.h"
#include "kbd.h"
#include "keymatrix.h"
#include "log.h"
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
    I_RESUME, I_SAVE, I_LOAD, I_DELETE, I_TAPES, I_RESET, I_VOLUME, I_BACKLIGHT,
    I_COUNT
};

#define TAPE_ROWS 11

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
} s;

static tapeio_entry_t s_list[ATOM_TAPE_LIST_MAX];

static void say(const char *fmt, const char *arg) {
    snprintf(s.status, sizeof s.status, fmt, arg);
}

/* ---- drawing ------------------------------------------------------------ */

static void draw_main(void) {
    char line[TEXT_COLS + 1];
    const char *slot_state = !s.card ? "NO CARD" : s.used[s.slot] ? "SAVED" : "EMPTY";

    for (int i = 0; i < I_COUNT; i++) {
        switch (i) {
        case I_RESUME: snprintf(line, sizeof line, " RESUME"); break;
        case I_SAVE:   snprintf(line, sizeof line, " SAVE SNAPSHOT   < SLOT %u >", s.slot + 1u); break;
        case I_LOAD:   snprintf(line, sizeof line, " LOAD SNAPSHOT   < SLOT %u >", s.slot + 1u); break;
        case I_DELETE: snprintf(line, sizeof line, " DELETE SNAPSHOT < SLOT %u >", s.slot + 1u); break;
        case I_TAPES:  snprintf(line, sizeof line, " TAPES..."); break;
        case I_RESET:  snprintf(line, sizeof line, " RESET (BREAK)"); break;
        case I_VOLUME: snprintf(line, sizeof line, " VOLUME          < %u >", s.set->volume); break;
        case I_BACKLIGHT:
            snprintf(line, sizeof line, " BACKLIGHT       < %u >", s.backlight / BKL_STEP);
            break;
        }
        textpage_line(s.vram, 2 + i, line, i == s.item);
    }

    snprintf(line, sizeof line, " SLOT %u: %s", s.slot + 1u, slot_state);
    textpage_line(s.vram, 11, line, false);

    const char *ins = tapeio_inserted();
    const char *base = strrchr(ins, '/');
    snprintf(line, sizeof line, " TAPE IN: %.21s", ins[0] ? (base ? base + 1 : ins) : "NONE");
    textpage_line(s.vram, 12, line, false);
}

static void draw_tapes(void) {
    char line[TEXT_COLS + 1];
    textpage_line(s.vram, 2, s.n_tapes ? " NAME             LOAD EXEC  LEN"
                                        : " NO .ATM FILES IN /ATOM/TAPES/", false);
    for (int r = 0; r < TAPE_ROWS; r++) {
        int i = s.tape_top + r;
        line[0] = 0;
        if (i == 0) {
            snprintf(line, sizeof line, " (EJECT)");
        } else if (i <= (int)s.n_tapes) {
            const tapeio_entry_t *e = &s_list[i - 1];
            bool in = strcmp(e->path, tapeio_inserted()) == 0;
            snprintf(line, sizeof line, "%c%-16.16s %04X %04X %4X", in ? '*' : ' ',
                     e->hdr.name[0] ? e->hdr.name : "\"\"", e->hdr.load, e->hdr.exec,
                     e->hdr.len);
        }
        textpage_line(s.vram, 3 + r, line, i == s.tape_sel);
    }
}

static void draw(void) {
    textpage_clear(s.vram);
    textpage_line(s.vram, 0, s.tapes ? " PICO-ATOM: TAPES" : " PICO-ATOM", true);
    if (s.tapes) draw_tapes();
    else draw_main();
    textpage_line(s.vram, 14, s.status, false);
    textpage_line(s.vram, 15, s.tapes ? " ENTER INSERTS  ESC BACK"
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
    s.tape_sel = 0;
    for (unsigned i = 0; i < s.n_tapes; i++) {
        if (strcmp(s_list[i].path, tapeio_inserted()) == 0) s.tape_sel = (int)i + 1;
    }
    s.tape_top = s.tape_sel >= TAPE_ROWS ? s.tape_sel - TAPE_ROWS + 1 : 0;
    s.status[0] = 0;
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
        if (s.item == I_SAVE || s.item == I_LOAD || s.item == I_DELETE) {
            s.slot = (s.slot + SNAPIO_SLOTS + (unsigned)dir) % SNAPIO_SLOTS;
        } else if (s.item == I_VOLUME) {
            int v = (int)s.set->volume + dir;
            s.set->volume = (unsigned)(v < 0 ? 0 : v > 8 ? 8 : v);
        } else if (s.item == I_BACKLIGHT) {
            set_backlight(dir);
        }
        break;
    }
    case PC_ENTER:
        s.status[0] = 0;
        switch (s.item) {
        case I_RESUME: s.done = true; break;
        case I_SAVE:   say(" SAVING...", ""); draw(); do_save(); break;
        case I_LOAD:   say(" LOADING...", ""); draw(); do_load(); break;
        case I_DELETE: do_delete(); break;
        case I_TAPES:  open_tapes(); break;
        case I_RESET:  s.m->cpu.reset_pending = true; s.done = true; break;
        }
        break;
    case PC_ESC:
        s.done = true;
        break;
    }
}

static void key_tapes(uint8_t c) {
    int last = (int)s.n_tapes;
    switch (c) {
    case PC_UP:   if (s.tape_sel > 0) s.tape_sel--; break;
    case PC_DOWN: if (s.tape_sel < last) s.tape_sel++; break;
    case PC_ENTER:
        if (s.tape_sel == 0) {
            tapeio_insert(NULL);
            say(" TAPE EJECTED", "");
        } else {
            const tapeio_entry_t *e = &s_list[s.tape_sel - 1];
            tapeio_insert(e->path);
            say(" IN: LOAD\"\" TAKES %.10s", e->hdr.name);
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

/* Presses only: releases and the MCU's held reports move nothing. Alt is
 * tracked so that Alt+M closes the menu as it opened it. */
static void keys(void) {
    uint8_t st, c;
    while (!s.done && kbd_pop(&st, &c)) {
        if (c == PICOCALC_KEY_ALT) { s.alt = st != KEY_EV_RELEASED; continue; }
        if (st != KEY_EV_PRESSED) continue;
        if (s.alt && (c == 'm' || c == 'M')) { s.done = true; break; }
        if (s.tapes) key_tapes(c);
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
