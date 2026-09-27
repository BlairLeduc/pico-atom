/* main.c — bring-up and the two cores' loops (design.md §4.2, §12.1, §17).
 *
 * Core 0 owns the 6502 and runs it in field-sized slices, split at the
 * flyback boundary, publishing a VRAM snapshot per field. Core 1 owns
 * the southbridge, the LCD and the SD card: it brings them up in
 * hardware-notes.md §10's order, loads the ROMs, and then presents
 * snapshots and polls the keyboard.
 *
 * Core 0 also owns audio (§9.4): each field's samples go into the PCM
 * queue, and waiting for room there is what paces the guest (§12.2). A
 * build with PICO_ATOM_AUDIO=0 paces on time_us_64() against an
 * absolute deadline instead — §12.2's fallback path.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <strings.h>

#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/rand.h"
#include "pico/stdlib.h"

#include "atom.h"
#include "audio.h"
#include "board.h"
#include "discio.h"
#include "display.h"
#include "hot.h"
#include "kbd.h"
#include "keymapio.h"
#include "keymatrix.h"
#include "lcd.h"
#include "log.h"
#include "mc6847.h"
#include "menu.h"
#include "pico_atom_version.h"
#include "roms.h"
#include "settingsio.h"
#include "snappool.h"
#include "southbridge.h"
#include "status.h"
#include "storage.h"
#include "tapeio.h"

#ifndef PICO_ATOM_AUDIO
#define PICO_ATOM_AUDIO 1
#endif

/* Characters received on UART1 are typed at the guest, so a hardware
 * run can be driven from the workstation that captures it (§12.3). The
 * probe's RX is already wired to GP5 for the log (hardware-notes.md
 * §2.7); nothing else reads it. */
#ifndef PICO_ATOM_UART_KEYS
#define PICO_ATOM_UART_KEYS 1
#endif

/* Turbo: while a UEF plays, the guest runs unpaced (design.md §11.3).
 * The tape is clocked in guest cycles, so a load finishes sooner by
 * exactly the headroom (§6.3), and nothing the guest can observe
 * changes. Its sound is dropped while it lasts — at twice the pitch it
 * would be noise, and a loader is silent — and the PCM queue is kept
 * topped up with silence instead, so the ring never runs dry. */
#ifndef PICO_ATOM_TURBO
#define PICO_ATOM_TURBO 1
#endif

/* M3's present-time measurement (design.md §8.4), kept for the perf pass
 * (M7) but not run on every boot: it holds the panel for seconds. */
#ifndef PICO_ATOM_MEASURE_PRESENT
#define PICO_ATOM_MEASURE_PRESENT 0
#endif

#if PICO_ATOM_HAVE_FONT
#include "mc6847_font.h"
#define FONT font_6847
#else
#define FONT NULL
#endif

/* The guest lives in .bss, not the heap: src/core/ has no allocator, and
 * keeping it static is what makes the §5 budget a link-time fact. */
static atom_t g_atom;
static keymatrix_t g_keys;   /* core 0's, like g_atom */
static snappool_t g_pool;
static spin_lock_t *g_pool_lock;

/* Core 1's counters, read by core 0's heartbeat. Each is a single 32-bit
 * word written by one core, so a torn read is not possible. */
static volatile struct {
    bool     ready;          /* bring-up finished; set after roms_ok */
    bool     roms_ok;        /* the machine can boot                */
    uint32_t key_events;
    uint32_t presents;
    uint32_t full_presents;
    uint32_t last_us;
    uint32_t max_us;
    int32_t  battery;        /* SB_REG_BAT's byte, -1 until read    */
    int32_t  temp_c;         /* the die, INT32_MIN until read       */
} g_c1 = { .battery = -1, .temp_c = INT32_MIN };

/* The perf line's host counters (design.md §13.1): core 0 closes a
 * window once a wall-clock second and writes its words here, each a
 * single 32-bit store; core 1 adds its own present time and drops. */
static volatile perf_line_t g_perf;

/* Bumped by core 1 after the Machine page's restart, while core 0 is
 * parked: core 0's counters kept against the old machine's clock start
 * again (§13.1). */
static volatile uint32_t g_power_ons;

/* ---- the machine handoff (§4.2, §11.1) ------------------------------ *
 * Card work belongs to core 1 and can outlast both the field and the
 * audio deadline, so it happens with the guest parked. Core 0 stops at a
 * field boundary, names the reason here, and from then on the machine is
 * core 1's until core 1 writes HANDOFF_NONE back. Core 0 keeps the PCM
 * queue fed with silence meanwhile, so the audio path neither underruns
 * nor loses its pacing (§12.2). */
#define HANDOFF_NONE 0u
#define HANDOFF_TAPE 1u   /* the CPU is stalled on OSLOAD/OSSAVE (tape.h) */
#define HANDOFF_MENU 2u   /* Alt+M (design.md §13)                      */
#define HANDOFF_DISC 3u   /* the FDC is waiting on sectors (i8271.h)    */
#define HANDOFF_PAUSE 4u  /* Alt+P (design.md §13.1)                    */

static volatile uint32_t g_handoff = HANDOFF_NONE;

/* What the settings file said at boot, and what a save from the menu
 * wrote since (design.md §11.6, §11.7). Core 1's. */
static settings_t g_file;

/* What the About page shows (design.md §13.1): the board, from boot, and
 * the ROMs, from boot and renewed by each restart. Core 1's. */
static board_info_t  g_board;
static roms_report_t g_roms;

/* Is the host at 300 MHz (design.md §3.2)? */
static bool host_fast(void) {
    return clock_get_hz(clk_sys) >= SETTINGS_HOST_MHZ_FAST * 1000000u;
}

static const char *machine_power_on(const atom_config_t *cfg, const char *utility,
                                    bool restart);
static const char *menu_restart(const atom_config_t *cfg, const char *utility) {
    return machine_power_on(cfg, utility, true);
}

/* Written by the menu on core 1 while core 0 is parked, applied by core
 * 0 once it has the machine back. */
static menu_settings_t g_settings = { .volume = 8, .turbo = true, .status = true,
                                      .file = &g_file, .board = &g_board, .roms = &g_roms,
                                      .restart = menu_restart };

/* ---- the snapshot handoff (§4.2): every transition under one lock ---- */

static int pool_claim(void) {
    uint32_t irq = spin_lock_blocking(g_pool_lock);
    int i = snappool_claim(&g_pool);
    spin_unlock(g_pool_lock, irq);
    return i;
}

static void pool_publish(int i) {
    uint32_t irq = spin_lock_blocking(g_pool_lock);
    snappool_publish(&g_pool, i);
    spin_unlock(g_pool_lock, irq);
}

static int pool_take(void) {
    uint32_t irq = spin_lock_blocking(g_pool_lock);
    int i = snappool_take(&g_pool);
    spin_unlock(g_pool_lock, irq);
    return i;
}

static void pool_release(int i) {
    uint32_t irq = spin_lock_blocking(g_pool_lock);
    snappool_release(&g_pool, i);
    spin_unlock(g_pool_lock, irq);
}

/* ---- core 1: bring-up, measurement, presentation --------------------- */

/* One scene, reused by every measurement and by the no-ROMs page; not a
 * snapshot from the pool, so it never holds a buffer core 0 might need. */
static uint8_t s_scene[ATOM_VRAM_SIZE];

#if PICO_ATOM_MEASURE_PRESENT

static uint32_t s_rng = 0x2545F491u;
static uint8_t rnd8(void) {
    s_rng ^= s_rng << 13; s_rng ^= s_rng >> 17; s_rng ^= s_rng << 5;
    return (uint8_t)s_rng;
}

typedef struct { uint32_t min, max, sum, n; } timing_t;

static void timing_add(timing_t *t, uint32_t us) {
    if (t->n == 0 || us < t->min) t->min = us;
    if (us > t->max) t->max = us;
    t->sum += us;
    t->n++;
}

/* One line per measurement, in one fixed format, so a UART capture can
 * be grepped straight into a file (hardware-notes.md §9.1). */
static void timing_report(const char *name, uint32_t pixels, const timing_t *t) {
    uint32_t mean = t->n ? t->sum / t->n : 0;
    /* px/us is Mpx/s; kept in hundredths to stay in integers. */
    uint32_t mpx100 = mean ? pixels * 100u / mean : 0;
    printf("  M3 present   : %-12s n=%lu px=%lu min=%lu mean=%lu max=%lu us  %lu.%02lu Mpx/s\n",
           name, (unsigned long)t->n, (unsigned long)pixels,
           (unsigned long)t->min, (unsigned long)mean, (unsigned long)t->max,
           (unsigned long)(mpx100 / 100u), (unsigned long)(mpx100 % 100u));
}

#define M3_REPEATS 20u

static void measure_full(const char *name, uint8_t mode) {
    timing_t t = { 0 };
    display_stats_t st = { 0 };
    for (unsigned i = 0; i < M3_REPEATS; i++) {
        display_invalidate();
        display_present(s_scene, mode, &st);
        timing_add(&t, st.us);
    }
    timing_report(name, st.pixels, &t);
}

/* Change `len` bytes at `offset`, then time the incremental present. */
static void measure_partial(const char *name, uint8_t mode, unsigned offset, unsigned len) {
    timing_t t = { 0 };
    display_stats_t st = { 0 };
    display_invalidate();
    display_present(s_scene, mode, NULL);
    for (unsigned i = 0; i < M3_REPEATS; i++) {
        for (unsigned k = 0; k < len; k++) s_scene[offset + k] ^= 0x01u;
        display_present(s_scene, mode, &st);
        timing_add(&t, st.us);
    }
    timing_report(name, st.pixels, &t);
}

/* design.md §17 M3: "present time measured and compared to §8.4's
 * estimate". Run while core 0 is executing the guest, because that is
 * the mode we ship (hardware-notes.md §9.1). */
static void measure_present(void) {
    printf("  M3 present   : §8.4 estimate: full 256x192 ~12.3 ms + ~0.5 ms fixed; "
           "one text line ~1.0 ms; one cell ~0.05 ms\n");

    /* The control quantity: the same rectangle with no row generation at
     * all. It is wire time plus the fixed cost, and it should not move
     * when the renderer changes. */
    timing_t t = { 0 };
    for (unsigned i = 0; i < M3_REPEATS; i++) {
        uint32_t t0 = time_us_32();
        lcd_fill(ATOM_SCREEN_X, ATOM_SCREEN_Y, ATOM_SCREEN_W, ATOM_SCREEN_H, 0x0000);
        timing_add(&t, time_us_32() - t0);
    }
    timing_report("control-fill", ATOM_SCREEN_W * ATOM_SCREEN_H, &t);

    const uint8_t alpha = 0;
    const uint8_t cg1 = VDG_AG | (0u << VDG_GM_SHIFT);
    const uint8_t rg6 = VDG_AG | (7u << VDG_GM_SHIFT);

    /* RG6: every row distinct, every byte its own LUT entry. */
    for (unsigned i = 0; i < sizeof(s_scene); i++) s_scene[i] = rnd8();
    measure_full("full-rg6", rg6);

    /* CG1: 64 rows generated, 192 sent — the vertical-reuse path. */
    measure_full("full-cg1", cg1);

    /* Alpha: a page of every glyph, some inverse, some SG6. */
    for (unsigned i = 0; i < 512; i++) s_scene[i] = (uint8_t)(i & 0xFFu);
    measure_full("full-alpha", alpha);
    measure_partial("alpha-line", alpha, 3u * 32u, 32u);
    measure_partial("alpha-cell", alpha, 5u * 32u + 7u, 1u);
}
#endif /* PICO_ATOM_MEASURE_PRESENT */

/* The MCU resets its own I2C bus after 2.5 s of silence (§6.1); the key
 * poll is what keeps it awake, at 30 Hz from this loop, in thread
 * context and never from a timer IRQ (design.md §10.2). */
#define KBD_POLL_US 33333u
/* The MCU refreshes its battery gauge every 20 s (hardware-notes.md §6). */
#define BAT_POLL_US 20000000u
/* The die temperature, once a heartbeat (hardware-notes.md §8.1). */
#define TEMP_POLL_US 5000000u

/* The heartbeat's battery and die: "87%, 31 C", "87% charging, 31 C",
 * with "?" for either before its first read or after a failed one.
 * Core 0's; the readings are core 1's. */
static const char *power_text(void) {
    static char text[48];
    int32_t b = g_c1.battery, t = g_c1.temp_c;
    char bat[16] = "?", die[16] = "?";
    if (b >= 0) snprintf(bat, sizeof bat, "%u%%%s", (unsigned)(b & 0x7F), b & 0x80 ? " charging" : "");
    if (t != INT32_MIN) snprintf(die, sizeof die, "%ld C", (long)t);
    snprintf(text, sizeof text, "%s, %s", bat, die);
    return text;
}

/* No ROMs: show why, and keep the southbridge awake, for ever. A blank
 * screen is the most expensive failure to debug on this hardware
 * (design.md §11.1). The fix is a card with the ROMs on it and a reset. */
static void __attribute__((noreturn)) no_roms(const roms_report_t *r) {
    roms_explain(r, s_scene);
    display_invalidate();
    display_present(s_scene, 0, NULL);
    for (;;) {
        uint8_t reply[2];
        (void)sb_read(SB_REG_KEY, reply);
        sleep_ms(1000);
    }
}

/* Loading a tape named on a layout's tapes line selects that layout
 * (design.md §10.5). Loading any other tape leaves the choice alone: a
 * game's loader may fetch its next part under another name. Core 1, with
 * core 0 parked. */
static void keys_for_tape(const char *name) {
    const keylayout_t *l = keymapio_for_tape(name);
    if (!l) return;
    memcpy(g_settings.keys_tape, name, sizeof g_settings.keys_tape);
    if (l == g_settings.layout) return;
    g_settings.layout = l;
    printf("  keymaps      : loading %s chose \"%s\"\n", name, l->name);
}

/* Core 0's: the settings file may turn turbo off (§11.3, §11.7). The
 * recorder is clocked in guest cycles as the player is, so a save runs
 * unpaced too. */
static bool turbo_now(void) {
    return PICO_ATOM_TURBO && g_settings.turbo &&
           (atom_cassette_playing(&g_atom) || atom_cassette_recording(&g_atom));
}

/* Core 1's: the status line from a snapshot's few bytes and the names of
 * what is in the deck and the drives, which are core 1's own (§8.2). The
 * presenter draws it only when the text changes. */
static void draw_status(const atom_status_t *st) {
    char text[ATOM_STATUS_COLS + 1];
    if (g_settings.status) {
        const char *drive[ATOM_FDC_DRIVES];
        for (unsigned d = 0; d < ATOM_FDC_DRIVES; d++) drive[d] = discio_inserted(d);
        status_format(st, tapeio_inserted(), drive, text);
    } else {
        memset(text, ' ', ATOM_STATUS_COLS);
        text[ATOM_STATUS_COLS] = 0;
    }
    display_status(text);
}

/* A bare name in the settings file is in the card's own folder for it;
 * a path from the root is taken as it stands (design.md §11.7). */
static const char *card_path(char out[ATOM_PATH_MAX], const char *dir, const char *v) {
    if (v[0] == '/') return v;
    int n = snprintf(out, ATOM_PATH_MAX, "%s/%s", dir, v);
    return n > 0 && n < (int)ATOM_PATH_MAX ? out : NULL;
}

/* What the settings file puts in the deck, the drives and the keys,
 * once the ROMs and the card's layouts are in. Core 1, with core 0
 * waiting and the card mounted. */
static void boot_media(const settings_t *b) {
    if (b->keys[0]) {
        int i = keymapio_find(b->keys);
        if (i >= 0) g_settings.layout = keymapio_get((unsigned)i);
        else settingsio_fail("keys", "no such layout");
    }
    char buf[ATOM_PATH_MAX];
    if (b->tape[0]) {
        const char *p = card_path(buf, SETTINGS_TAPE_DIR, b->tape);
        const char *err = p ? tapeio_insert(&g_atom, p) : "path too long";
        if (err) settingsio_fail("tape", err);
        else printf("  tape         : %s in the deck\n", p);
    }
    for (unsigned d = 0; d < SETTINGS_DRIVES; d++) {
        if (!b->drive[d][0]) continue;
        const char *p = card_path(buf, SETTINGS_DISC_DIR, b->drive[d]);
        const char *err = p ? discio_insert(&g_atom, d, p) : "path too long";
        char what[8];
        snprintf(what, sizeof what, "drive%u", d);
        if (err) settingsio_fail(what, err);
        else printf("  disc         : %s in drive %u\n", p, d);
    }
}

/* The machine powered on with `cfg` and `utility` (design.md §13.1): the
 * boot's configuration step, and the Machine page's restart. Core 1, with
 * core 0 waiting at boot or parked in the menu, so the machine is core
 * 1's to write (§4.2); the card is mounted.
 *
 * A restart checks the card first, in two passes as a snapshot loads
 * (§11.5): every file the new machine needs is read and hashed, and a
 * missing or wrong one refuses the restart and leaves the machine
 * running as it was. Then it is a power-on: the tape goes back in the
 * deck at its start and the discs back in their drives, as at boot
 * (§11.7), and the layout in force stays. At boot there is no machine to
 * keep, so nothing is checked: a missing ROM is the no-ROMs page.
 *
 * The old machine is not kept for the second pass: its ROMs are in its
 * ram[], and a second atom_t is ~70 KiB (§5). A card changed between the
 * passes, a few milliseconds apart, is the no-ROMs page, as at boot,
 * rather than a guest running without its kernel. NULL, or why not. */
static const char *machine_power_on(const atom_config_t *cfg, const char *utility,
                                    bool restart) {
    static char why[40];
    char tape[ATOM_PATH_MAX], drive[ATOM_FDC_DRIVES][ATOM_PATH_MAX];
    if (restart) {
        const char *err = roms_check(cfg, utility);
        if (err) {
            printf("  machine      : not restarted: %s\n", err);
            snprintf(why, sizeof why, "%.20s: NOT RESTARTED", err);
            return why;
        }
        snprintf(tape, sizeof tape, "%s", tapeio_inserted());
        for (unsigned d = 0; d < ATOM_FDC_DRIVES; d++)
            snprintf(drive[d], sizeof drive[d], "%s", discio_inserted(d));
    }

    /* 4 MHz only on a host at 300 (design.md §3.2, §12.1): at 150 it
     * would underrun, so it runs at 2. */
    atom_config_t run = *cfg;
    if (atom_clock_mhz(&run) == 4u && !host_fast()) run.clock_mhz = 2u;
    cfg = &run;
    atom_init(&g_atom, cfg);
    bool ok = roms_load(&g_atom, utility, &g_roms);
    snprintf(g_settings.utility, sizeof g_settings.utility, "%s", utility);
    atom_seed_rnd(&g_atom, get_rand_64());   /* power-on RAM is not zero (§7.2) */
    atom_reset(&g_atom);                     /* the vectors arrived with the kernel */
    /* The beeper's rate is the PWM's, which core 0 finds at audio_init;
     * before that, at boot, core 0 sets it itself. */
    uint32_t num = 0, den = 0;
#if PICO_ATOM_AUDIO
    audio_rate(&num, &den);
#endif
    if (num) atom_audio_set_rate(&g_atom, num, den);
    printf("  machine      : %s, %s KiB, %u MHz, %s, utility %s\n",
           restart ? "restarted" : "powered on", cfg->upper_ram ? "32" : "16",
           atom_clock_mhz(cfg), cfg->atomdos ? "AtomDOS" : "no DOS",
           utility[0] ? utility : "none");

    if (restart) {
        roms_log(&g_roms);
        if (!ok) no_roms(&g_roms);
        const char *err = tape[0] ? tapeio_insert(&g_atom, tape) : NULL;
        if (err) printf("  tape         : %s not back in: %s\n", tape, err);
        for (unsigned d = 0; d < ATOM_FDC_DRIVES; d++) {
            err = drive[d][0] ? discio_insert(&g_atom, d, drive[d]) : NULL;
            if (err) printf("  disc         : %s not back in drive %u: %s\n", drive[d], d, err);
        }
        __dmb();
        g_power_ons++;
    }
    return ok ? NULL : "NO ROMS";
}

/* Pause (design.md §13.1): the guest is parked, its last frame stays on
 * the panel, the status line says PAUSED whether or not it is on, and the
 * backlight goes to its lowest step. Any key resumes, and is not typed;
 * Alt+M goes to the menu instead, the backlight restored first. The card
 * is not mounted and no page is drawn. The keyboard is polled at 30 Hz,
 * which keeps the MCU's bus watchdog fed (hardware-notes.md §6.1).
 * Returns true for the menu. */
#define BKL_LOWEST 16u

static bool pause_run(void) {
    char line[ATOM_STATUS_COLS + 1];
    status_paused_format(line);
    display_status(line);

    /* The level may be the southbridge's own, not the settings' or the
     * menu's (§11.7), so it is read, and written back on resume. */
    uint8_t r[2] = {0};
    bool read = sb_read(SB_REG_BKL, r) == SB_OK;
    uint8_t level = read ? r[1] : BKL_LOWEST;
    bool dimmed = read && level != BKL_LOWEST && sb_write(SB_REG_BKL, BKL_LOWEST, NULL) == SB_OK;
    printf("  pause        : paused, backlight %s\n",
           !read ? "unread, left" : dimmed ? "dimmed" : "already lowest");

    /* It was asked for with Alt held. */
    bool alt = true, menu = false, done = false;
    uint32_t last_poll = time_us_32();
    while (!done) {
        if (time_us_32() - last_poll >= KBD_POLL_US) {
            last_poll = time_us_32();
            g_c1.key_events += kbd_poll();
            uint8_t st, c;
            while (!done && kbd_pop(&st, &c)) {
                if (c == PICOCALC_KEY_ALT) { alt = st != KEY_EV_RELEASED; continue; }
                if (st != KEY_EV_PRESSED) continue;
                /* A modifier alone resumes nothing, so Alt+M can be had;
                 * nor does the pause chord's own auto-repeat. */
                if (c == PICOCALC_KEY_CTRL || c == PICOCALC_KEY_SHIFT_L ||
                    c == PICOCALC_KEY_SHIFT_R) continue;
                if (alt && (c == 'P' || c == 'p')) continue;
                menu = alt && (c == 'M' || c == 'm');
                done = true;
            }
        }
        log_pump();
        busy_wait_us_32(500);
    }

    if (dimmed) (void)sb_write(SB_REG_BKL, level, NULL);
    printf("  pause        : resumed%s\n", menu ? " into the menu" : "");
    return menu;
}

/* The perf line (design.md §13.1): core 0's window, and core 1's own
 * present time and drops over the same wall-clock second. Drawn only when
 * the text changes, so at most once a second. */
static void draw_perf(uint32_t present_max, uint32_t dropped) {
    char text[ATOM_STATUS_COLS + 1];
    if (g_settings.perf) {
        perf_line_t p;
        p.busy1000 = g_perf.busy1000;
        p.head100 = g_perf.head100;
        p.underruns = g_perf.underruns;
        p.late = g_perf.late;
        p.present_us = present_max;
        p.dropped = dropped;
        status_perf_format(&p, text);
    } else {
        memset(text, ' ', ATOM_STATUS_COLS);
        text[ATOM_STATUS_COLS] = 0;
    }
    display_perf(text);
}

static void core1_main(void) {
    printf("  core 1       : up\n");

    /* 1. Southbridge first: a dead bus is the first thing to know about
     *    (hardware-notes.md §10). */
    uint32_t i2c_hz = sb_init();
    uint8_t r[2] = { 0, 0 };
    sb_status_t st_ver = sb_read(SB_REG_VER, r);
    uint8_t ver = r[1];
    sb_status_t st_bkl = sb_read(SB_REG_BKL, r);
    uint8_t bkl = r[1];
    printf("  southbridge  : i2c1 %lu Hz; VER %s 0x%02x; backlight %s %u\n",
           (unsigned long)i2c_hz,
           st_ver == SB_OK ? "=" : "FAILED", ver,
           st_bkl == SB_OK ? "=" : "FAILED", bkl);

    /* 2. LCD. The rate is as configured, not as measured on SCK (§4.2). */
    uint32_t baud = lcd_init();
    printf("  lcd          : spi1 configured at %lu Hz\n", (unsigned long)baud);

    display_init(FONT);

    /* 3. The settings file, before the ROMs, because some of it is the
     *    machine: RAM, the clock, AtomDOS and the utility ROM decide what
     *    loads (design.md §11.7). Core 0 is waiting, so the machine is
     *    core 1's to configure. */
    settings_t *boot = &g_file;
    int mount_error = storage_mount();
    if (mount_error == 0) settingsio_load(boot);
    else settings_default(boot);
    g_settings.mono      = boot->mono;
    g_settings.border    = boot->border;
    g_settings.dark_bg   = boot->dark_bg;
    g_settings.status    = boot->status;
    g_settings.volume    = boot->volume;
    g_settings.backlight = boot->backlight;
    g_settings.turbo     = boot->turbo;
    g_settings.perf      = boot->perf;
#ifdef PICO_ATOM_BOOT_PERF
    g_settings.perf      = true;   /* over the file's */
#endif
    display_set_look(g_settings.mono, g_settings.border, g_settings.dark_bg);
    if (boot->backlight) (void)sb_write(SB_REG_BKL, (uint8_t)(boot->backlight * 16u), NULL);
#if PICO_ATOM_MEASURE_PRESENT
    display_test_pattern();
    printf("  M3 pattern   : white border on (32,64)-(287,255); corners "
           "red TL, green TR, blue BL, yellow BR; outside black\n");
    sleep_ms(5000);
    measure_present();
#endif
    display_invalidate();

    /* 4. The machine and its ROMs, while core 0 waits: loading writes the
     *    ROMs into the machine, which is core 1's until it says so
     *    (roms.h, §13.1). */
#ifdef PICO_ATOM_BOOT_CLOCK
    boot->machine.clock_mhz = PICO_ATOM_BOOT_CLOCK;   /* over the file's */
#endif
    if (atom_clock_mhz(&boot->machine) == 4u && !host_fast())
        settingsio_fail("clock", "4 MHz needs host_clock 300");
    bool ok = false;
    if (mount_error == 0) {
        ok = machine_power_on(&boot->machine, boot->utility, false) == NULL;
    } else {
        memset(&g_roms, 0, sizeof g_roms);
        g_roms.mount_error = mount_error;
    }
    roms_log(&g_roms);
    /*    A utility ROM the file names and the card lacks is its problem;
     *    the default, missing, is an empty socket, as it always was. */
    if (ok && boot->utility[0] && strcasecmp(boot->utility, SETTINGS_UTILITY) != 0) {
        if (g_roms.slot[ROM_UTILITY] == ROM_MISSING) settingsio_fail("utility", "no such file");
        else if (g_roms.slot[ROM_UTILITY] == ROM_BAD_SIZE) settingsio_fail("utility", "not 4096 bytes");
    }

    /*    The card's game keymaps too, so that the first tape load can
     *    choose one (design.md §10.5). The menu reads them again when it
     *    opens. */
    if (ok) {
        keymapio_scan();
        boot_media(boot);
#ifdef PICO_ATOM_BOOT_TAPE
        /* In the deck, stopped, for a run driven over the UART, where
         * the menu cannot be reached. Over the settings file's. */
        const char *err = tapeio_insert(&g_atom, PICO_ATOM_BOOT_TAPE);
        printf("  tape         : boot tape %s: %s\n", PICO_ATOM_BOOT_TAPE,
               err ? err : "in the deck");
#endif
#ifdef PICO_ATOM_BOOT_NEW_TAPE
        /* A blank to record onto, for the same kind of run (§11.3). */
        const char *nerr = tapeio_new(&g_atom);
        printf("  tape         : boot new tape: %s\n", nerr ? nerr : tapeio_inserted());
#endif
#ifdef PICO_ATOM_BOOT_DISC
        /* In drive 0, for a run driven over the UART, where the menu
         * cannot be reached. Over the settings file's. */
        const char *derr = discio_insert(&g_atom, 0, PICO_ATOM_BOOT_DISC);
        printf("  disc         : boot disc %s: %s\n", PICO_ATOM_BOOT_DISC,
               derr ? derr : "in drive 0");
#endif
    }
    if (mount_error == 0) storage_unmount();
    g_c1.roms_ok = ok;
    __dmb();
    g_c1.ready = true;
    if (!ok) no_roms(&g_roms);

    /* Live: present the newest snapshot, drop superseded ones (§12.2),
     * and poll the keyboard at 30 Hz, and the battery gauge as often as
     * the MCU refreshes it, for the heartbeat: whether it is charging is
     * how a log shows the soak ran on battery (§15.3). The die's
     * temperature too, once a heartbeat (§12.3). */
    uint32_t last_poll = time_us_32();
    uint32_t last_bat = last_poll - BAT_POLL_US, last_temp = last_poll - TEMP_POLL_US;
    board_temp_init();
    /* The perf line's second, core 1's half of it (§13.1). */
    uint32_t sec_start = last_poll, sec_max_us = 0, sec_dropped = g_pool.dropped;
    for (;;) {
        int i = pool_take();
        if (i >= 0) {
            display_stats_t st;
            display_present(g_pool.buf[i].vram, g_pool.buf[i].mode, &st);
            atom_status_t line = g_pool.buf[i].status;
            pool_release(i);
            draw_status(&line);

            g_c1.presents++;
            if (st.full) g_c1.full_presents++;
            g_c1.last_us = st.us;
            if (st.us > g_c1.max_us) g_c1.max_us = st.us;
            if (st.us > sec_max_us) sec_max_us = st.us;
        }

        log_pump();

        if (g_handoff != HANDOFF_NONE) {
            __dmb();
            if (g_handoff == HANDOFF_TAPE) {
                char loaded[ATOM_ATM_NAME_LEN + 1];
                if (tapeio_serve(&g_atom, loaded)) keys_for_tape(loaded);
            } else if (g_handoff == HANDOFF_DISC) {
                (void)discio_serve(&g_atom);
            } else if (g_handoff == HANDOFF_PAUSE) {
                if (pause_run()) menu_run(&g_atom, &g_settings, s_scene);
            } else {
                menu_run(&g_atom, &g_settings, s_scene);
            }
            __dmb();
            g_handoff = HANDOFF_NONE;
        }

        if (time_us_32() - sec_start >= 1000000u) {
            sec_start = time_us_32();
            draw_perf(sec_max_us, g_pool.dropped - sec_dropped);
            sec_max_us = 0;
            sec_dropped = g_pool.dropped;
        }

        if (time_us_32() - last_poll >= KBD_POLL_US) {
            last_poll = time_us_32();
            g_c1.key_events += kbd_poll();
        }
        if (time_us_32() - last_bat >= BAT_POLL_US) {
            last_bat = time_us_32();
            uint8_t r[2];
            g_c1.battery = sb_read(SB_REG_BAT, r) == SB_OK ? r[1] : -1;
        }
        if (time_us_32() - last_temp >= TEMP_POLL_US) {
            last_temp = time_us_32();
            g_c1.temp_c = board_temp_c();
        }

        /* A hardware-timer wait between iterations rather than spinning
         * on shared state (design.md §4.2). A busy wait, not sleep_us:
         * sleep_us sets an alarm in the default pool, whose IRQ belongs
         * to core 0, so every iteration here interrupted the guest
         * (hardware-notes.md §9.7). */
        if (i < 0) busy_wait_us_32(20);
    }
}

/* ---- core 0: the guest ------------------------------------------------- */

#if PICO_ATOM_UART_KEYS
/* One character from UART1 as the PicoCalc would send it: a press and a
 * release, with Shift held around the characters that need it — the
 * same events test_boot types with (§10.2). Taken only while keymatrix
 * has room for all four events, so a fast sender loses characters in
 * the UART FIFO rather than in the replay queue; send slowly. */
static void uart_keys(void) {
    if (g_keys.q_len + g_keys.n_open + 8u > ATOM_KEY_EVENT_QUEUE) return;
    int ch = getchar_timeout_us(0);
    if (ch == PICO_ERROR_TIMEOUT) return;

    uint8_t c = (uint8_t)ch;
    if (c == 0x1Eu) {
        /* RS: the deck's play/stop, as the menu's (§13), so a run driven
         * from the workstation can play a tape. No key sends it. */
        atom_cassette_play(&g_atom, !atom_cassette_playing(&g_atom));
        log_printf("  cassette     : %s over the UART\n",
                   atom_cassette_playing(&g_atom) ? "playing" : "stopped");
        return;
    }
    if (c == '\r') c = '\n';
    if (c == 0x1Bu) {
        /* ESC, so a run can stop one BASIC program and type the next. */
        uint8_t esc;
        if (keymap_picocalc_key_named("esc", &esc)) {
            keymatrix_event(&g_keys, KEY_EV_PRESSED, esc);
            keymatrix_event(&g_keys, KEY_EV_RELEASED, esc);
        }
        return;
    }
    if (c >= 1u && c <= 26u && c != '\n') {
        /* Control characters arrive as CTRL + letter. */
        keymatrix_event(&g_keys, KEY_EV_PRESSED, PICOCALC_KEY_CTRL);
        keymatrix_event(&g_keys, KEY_EV_PRESSED, (uint8_t)(c + 'a' - 1u));
        keymatrix_event(&g_keys, KEY_EV_RELEASED, (uint8_t)(c + 'a' - 1u));
        keymatrix_event(&g_keys, KEY_EV_RELEASED, PICOCALC_KEY_CTRL);
        return;
    }
    bool shifted = c != 0 && strchr("!\"#$%&'()=<+*>?|{}~", c) != NULL;
    if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32u);   /* unshifted = capitals */
    else if (c >= 'a' && c <= 'z') { c = (uint8_t)(c - 32u); shifted = true; }
    if (shifted) keymatrix_event(&g_keys, KEY_EV_PRESSED, PICOCALC_KEY_SHIFT_L);
    keymatrix_event(&g_keys, KEY_EV_PRESSED, c);
    if (shifted) keymatrix_event(&g_keys, KEY_EV_RELEASED, PICOCALC_KEY_SHIFT_L);
    keymatrix_event(&g_keys, KEY_EV_RELEASED, keymap_picocalc_canonical(c));
}
#endif

#if PICO_ATOM_AUDIO
/* The menu's volume beep: the MOS bell's pitch and a guest square
 * wave's swing after the DC blocker (BEEPER_FULL_SCALE / 2), so it is
 * as loud as the guest will be. */
#define BEEP_HZ  388u
#define BEEP_MS  120u
#define BEEP_AMP (BEEPER_FULL_SCALE / 2)
#endif

/* Hand the machine to core 1 and wait for it back (g_handoff). While
 * it is away the PCM queue is fed silence, or the menu's beep. */
static void park(uint32_t why) {
    __dmb();
    g_handoff = why;
#if PICO_ATOM_AUDIO
    static const int16_t silence[128];
    uint32_t num, den;
    audio_rate(&num, &den);
    const uint32_t half = num / (den * 2u * BEEP_HZ);
    uint32_t beeps = g_settings.beep, left = 0, phase = 0;
    while (g_handoff != HANDOFF_NONE) {
        if (g_settings.beep != beeps) {
            beeps = g_settings.beep;
            __dmb();
            audio_set_volume(g_settings.volume * 32u);
            left = (uint32_t)((uint64_t)num * BEEP_MS / (1000u * (uint64_t)den));
            phase = 0;
        }
        if (!left) { audio_push(silence, 128u); continue; }
        int16_t tone[128];
        size_t n = left < 128u ? left : 128u;
        for (size_t i = 0; i < n; i++, phase++) {
            if (phase == 2u * half) phase = 0;
            tone[i] = phase < half ? BEEP_AMP : -BEEP_AMP;
        }
        audio_push(tone, n);
        left -= (uint32_t)n;
    }
#else
    while (g_handoff != HANDOFF_NONE) sleep_us(100);
#endif
    __dmb();
}

/* Park for the menu or a pause, which own the keyboard while they last
 * (§13, §13.1): the keys, the chord's own releases among them, were core
 * 1's, so the held set starts again empty, and the key that resumes a
 * pause is never typed. True if the Machine page restarted the machine
 * meanwhile. */
static bool park_for_ui(uint32_t why) {
    uint32_t power_ons = g_power_ons;
    park(why);
    keymatrix_init(&g_keys);
    keymatrix_set_layout(&g_keys, g_settings.layout);
#if PICO_ATOM_AUDIO
    audio_set_volume(g_settings.volume * 32u);
#endif
    return g_power_ons != power_ons;
}

/* The host clock the card asks for (design.md §3.2, §11.7), read before
 * anything is brought up, because everything derives its rate from the
 * clock it finds. Nothing is printed: stdio is not up, and core 1 reads
 * the file again, and says what it finds, once it is. */
static unsigned boot_host_mhz(void) {
#ifdef PICO_ATOM_BOOT_HOST_MHZ
    return PICO_ATOM_BOOT_HOST_MHZ;   /* over the file's */
#else
    if (storage_mount() != 0) return SETTINGS_HOST_MHZ;
    settingsio_load(&g_file);
    storage_unmount();
    return g_file.host_mhz;
#endif
}

int main(void) {
    /* 150 MHz first, for the card; then the card's clock, before stdio,
     * so the UART's divider is worked out from the clock that stays. */
    bool clocks_ok = board_init_clocks(SETTINGS_HOST_MHZ);
    unsigned host_mhz = clocks_ok ? boot_host_mhz() : SETTINGS_HOST_MHZ;
    if (clocks_ok && host_mhz != SETTINGS_HOST_MHZ) clocks_ok = board_init_clocks(host_mhz);
    stdio_init_all();

    board_identify(&g_board);

    /* A startup banner plus consecutive heartbeats is more useful boot
     * evidence than a single line (hardware-notes.md §2.7). */
    board_log_banner(&g_board);
    printf("  firmware     : %s\n", PICO_ATOM_VERSION);
    if (!clocks_ok) {
        printf("  WARNING: clk_sys is not at the %u MHz asked for; SPI and audio "
               "rates will not be the ones this build assumes\n", host_mhz);
    }

    /* Core 1 configures the machine from the settings file and loads
     * the ROMs into it (settingsio.h, roms.h). */
    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&g_atom, &cfg);
#if !PICO_ATOM_HAVE_FONT
    printf("  WARNING: no MC6847 character ROM supplied; alpha mode will "
           "draw placeholder cells (design.md §16)\n");
#endif

    snappool_init(&g_pool);
    g_pool_lock = spin_lock_init(spin_lock_claim_unused(true));
    keymatrix_init(&g_keys);
    multicore_launch_core1(core1_main);

    /* Core 1 loads the ROMs off the card into g_atom; until it says so,
     * the machine is its to write (roms.h). The build ships none
     * (design.md §1, §11.1). */
    while (!g_c1.ready) sleep_ms(1);
    __dmb();
    printf("  guest        : %u MHz, %u cycles/field at %u Hz, FS low %u, %u KiB address space\n",
           atom_clock_mhz(&g_atom.cfg), (unsigned)atom_cycles_per_field(&g_atom),
           ATOM_FIELD_HZ, (unsigned)g_atom.field_fs_low,
           (unsigned)(ATOM_ADDR_SPACE / 1024u));
    if (!g_c1.roms_ok) {
        printf("  guest        : not started — no ROMs (the panel says which)\n");
        for (;;) sleep_ms(1000);
    }
    /* Audio last in bring-up order (hardware-notes.md §10), and after the
     * LCD has claimed its fixed DMA channel on core 1. */
#if PICO_ATOM_AUDIO
    audio_init();
    uint32_t rate_num, rate_den;
    audio_rate(&rate_num, &rate_den);
    atom_audio_set_rate(&g_atom, rate_num, rate_den);
    printf("  audio        : PWM GP26/GP27, %lu/%lu Hz (%lu.%02lu kHz), "
           "%u guest cycles per %u samples, ring %u slots\n",
           (unsigned long)rate_num, (unsigned long)rate_den,
           (unsigned long)(rate_num / rate_den / 1000u),
           (unsigned long)(rate_num / rate_den % 1000u / 10u),
           (unsigned)g_atom.beeper.num, (unsigned)g_atom.beeper.den,
           (unsigned)ATOM_DMA_RING_SLOTS);
    audio_set_volume(g_settings.volume * 32u);   /* the settings file's */
#else
    printf("  audio        : disabled; pacing on the microsecond timer\n");
#endif
    keymatrix_set_layout(&g_keys, g_settings.layout);

    /* Core 1 seeded RND and reset the machine once the kernel was in
     * (machine_power_on). */
    printf("  guest        : started at #%04X; hot code in SRAM to tier %u "
           "(hot.h)\n", g_atom.cpu.pc, (unsigned)PICO_ATOM_RAM_TIER);

    uint32_t field = 0;
#if PICO_ATOM_AUDIO
    static int16_t pcm[ATOM_AUDIO_BUF_LEN];
    audio_stats_t au_last = { 0 };
#else
    uint32_t late = 0;
    const uint32_t field_us = 1000000u / ATOM_FIELD_HZ;
    absolute_time_t next = get_absolute_time();
#endif
    uint64_t hb_us = time_us_64(), hb_cycles = g_atom.cpu.cycles;
    uint64_t hb_insns = g_atom.instructions;
    uint32_t run_us = 0;   /* inside atom_run_field since the heartbeat */
    uint32_t turbo_fields = 0;
    /* The status line's (§8.2): the tape's position and the turbo ratio
     * change the text at most once a wall-clock second. */
    atom_status_t shown = { 0 };
    uint64_t sec_us = hb_us, sec_cycles = hb_cycles;
    uint32_t sec_run_us = 0;   /* inside atom_run_field this second (§13.1) */
    bool sec_turbo = false;
    const uint32_t clk_mhz = clock_get_hz(clk_sys) / 1000000u;
    for (;;) {
#if !PICO_ATOM_AUDIO
        /* Absolute, not incremental, so a late field does not accumulate
         * (§12.2). Turbo runs unpaced and restarts the schedule after. */
        if (turbo_now()) {
            next = get_absolute_time();
        } else {
            next = delayed_by_us(next, field_us);
            if (absolute_time_diff_us(get_absolute_time(), next) < 0) late++;
            sleep_until(next);
        }
#endif

#if PICO_ATOM_UART_KEYS
        uart_keys();
#endif

        /* Keys first, so the matrix the guest scans this field is the
         * one the events describe (§10.2). */
        uint8_t kstate, kcode;
        while (kbd_pop(&kstate, &kcode)) keymatrix_event(&g_keys, kstate, kcode);
        keymatrix_field(&g_keys, &g_atom);
        /* The menu pauses the guest (§13), and so does Pause (§13.1):
         * both park it, with the PCM queue fed silence, so everything
         * counted in guest cycles stops with it. */
        if (g_keys.menu_request || g_keys.pause_request) {
            if (park_for_ui(g_keys.menu_request ? HANDOFF_MENU : HANDOFF_PAUSE)) {
                /* A new machine, its clock started again (§13.1). */
                hb_cycles = sec_cycles = g_atom.cpu.cycles;
                hb_insns = g_atom.instructions;
                shown = (atom_status_t){ 0 };
            }
        }

        uint32_t t0 = time_us_32();
        atom_run_field(&g_atom);
        uint32_t ran_us = time_us_32() - t0;
        run_us += ran_us;
        sec_run_us += ran_us;

        bool turbo = turbo_now();
        if (turbo) turbo_fields++;

        atom_status_t now_st;
        atom_status(&g_atom, &now_st);
        uint64_t now_us = time_us_64();
        if (now_st.deck != shown.deck) shown.percent = now_st.percent;
        if (now_us - sec_us >= 1000000u) {
            /* Guest cycles per microsecond, over the clock, is times
             * real time. */
            const unsigned mhz = atom_clock_mhz(&g_atom.cfg);
            uint64_t guest = g_atom.cpu.cycles - sec_cycles, wall = now_us - sec_us;
            uint64_t tenths = guest * 10u / (wall * mhz);
            shown.turbo10 = sec_turbo ? (uint8_t)(tenths > 255u ? 255u : tenths) : 0u;
            shown.percent = now_st.percent;
            /* The perf line's window (§13.1): whole words, each written
             * once, for core 1 to read. */
            g_perf.busy1000 = (uint32_t)((uint64_t)sec_run_us * 1000u / wall);
            g_perf.head100 = (uint32_t)(guest * 100u / (((uint64_t)sec_run_us + 1u) * mhz));
#if PICO_ATOM_AUDIO
            audio_stats_t pst;
            audio_stats(&pst, false);
            g_perf.underruns = pst.underrun_samples;
            g_perf.late = pst.late_refills;
#endif
            sec_us = now_us;
            sec_cycles = g_atom.cpu.cycles;
            sec_run_us = 0;
            sec_turbo = true;
        }
        if (!turbo) {
            sec_turbo = false;
            shown.turbo10 = 0;
        }
        shown.deck = now_st.deck;
        shown.heads = now_st.heads;
        shown.errors = now_st.errors;

        int i = pool_claim();
        if (i >= 0) {
            snapshot_t *s = &g_pool.buf[i];
            memcpy(s->vram, atom_vram(&g_atom), ATOM_VRAM_SIZE);
            s->mode = atom_vdg_mode(&g_atom);
            s->field = field;
            s->status = shown;
            pool_publish(i);
        }
#if PICO_ATOM_AUDIO
        size_t n = atom_audio_drain(&g_atom, pcm, ATOM_AUDIO_BUF_LEN);
        if (turbo) {
            /* Never block: keep the queue at its start depth with
             * silence, a field's worth of slack above the low water
             * that a paced field leaves (§12.2). */
            static const int16_t silence[256];
            size_t room = audio_room();
            size_t keep = ATOM_PCM_QUEUE_LEN - ATOM_PCM_QUEUE_START;
            while (room > keep) {
                size_t k = room - keep < 256u ? room - keep : 256u;
                audio_push(silence, k);
                room -= k;
            }
        } else {
            /* Blocks while the queue is full: this is the throttle (§12.2). */
            audio_push(pcm, n);
        }
#else
        /* The core makes samples regardless; discard them, or its buffer
         * fills and every later one is counted as an overflow. */
        int16_t discard[64];
        while (atom_audio_drain(&g_atom, discard, 64u) > 0) {}
#endif

        /* The CPU has stopped on a tape call (§11.2): the file is core
         * 1's to find, and the machine is core 1's while it does. */
        if (atom_tape_pending(&g_atom)) {
            park(HANDOFF_TAPE);
            /* A key already down keeps its binding (keymatrix.h). */
            keymatrix_set_layout(&g_keys, g_settings.layout);
        }

        /* The FDC is waiting on sectors (§11.4). The CPU runs on while
         * it waits, busy, as it would behind a real drive; guest time
         * stands still while core 1 reads the card. */
        if (atom_disc_request(&g_atom)) park(HANDOFF_DISC);

        if (++field % (ATOM_FIELD_HZ * 5u) == 0) {
            /* Real-time ratio, guest seconds per wall second, in
             * thousandths: the headline number (§12.3). */
            uint64_t now = time_us_64();
            uint64_t guest = g_atom.cpu.cycles - hb_cycles;
            uint32_t rt1000 = (uint32_t)(guest * 1000u * 1000000u / g_atom.cpu_hz /
                                         (now - hb_us + 1u));
            const mc6847_mode_info_t *vdg = mc6847_mode_info(atom_vdg_mode(&g_atom));
            log_printf("  heartbeat    : %lu fields, rt %lu.%03lu, %llu guest cycles, "
                   "%u undoc op(s), VDG %s | %s %lu presents (%lu full, "
                   "%lu dropped), last %lu us, max %lu us, i2c errors %lu | "
                   "keys %lu (%lu lost), tape calls %lu, disc sectors %lu read %lu written | "
                   "battery %s\n",
                   (unsigned long)field,
                   (unsigned long)(rt1000 / 1000u), (unsigned long)(rt1000 % 1000u),
                   (unsigned long long)g_atom.cpu.cycles,
                   (unsigned)g_atom.cpu.undoc_count, vdg->name,
                   g_c1.ready ? "live" : "bring-up",
                   (unsigned long)g_c1.presents, (unsigned long)g_c1.full_presents,
                   (unsigned long)g_pool.dropped,
                   (unsigned long)g_c1.last_us, (unsigned long)g_c1.max_us,
                   (unsigned long)sb_error_count(),
                   (unsigned long)g_c1.key_events,
                   (unsigned long)(kbd_overflows() + g_keys.dropped),
                   (unsigned long)g_atom.tape.served,
                   (unsigned long)g_atom.fdc.sectors_read,
                   (unsigned long)g_atom.fdc.sectors_written, power_text());
            /* Where core 0's time goes (§12.3, design.md §6.3). The
             * guest is paced, so rt above reads 1.000 whatever the code
             * costs; the cost is the time spent inside the guest.
             * Headroom is guest cycles per microsecond of it — how many
             * times real time the guest would run unpaced. */
            uint64_t insns = g_atom.instructions - hb_insns;
            uint32_t busy1000 = (uint32_t)((uint64_t)run_us * 1000u / (now - hb_us + 1u));
            /* Over the clock, so it is times real time at 2 MHz too. */
            uint32_t head100 = (uint32_t)(guest * 100u /
                                          (((uint64_t)run_us + 1u) * atom_clock_mhz(&g_atom.cfg)));
            uint32_t hpi10 = (uint32_t)((uint64_t)run_us * clk_mhz * 10u / (insns + 1u));
            uint32_t gpi100 = (uint32_t)(guest * 100u / (insns + 1u));
            log_printf("  perf         : tier %u, %u MHz, guest %lu.%lu%% of wall, headroom "
                   "%lu.%02lux, %lu.%lu host cycles/insn, %lu.%02lu guest cycles/insn, "
                   "%llu insns, %lu guest cycles/s\n",
                   (unsigned)PICO_ATOM_RAM_TIER, atom_clock_mhz(&g_atom.cfg),
                   (unsigned long)(busy1000 / 10u), (unsigned long)(busy1000 % 10u),
                   (unsigned long)(head100 / 100u), (unsigned long)(head100 % 100u),
                   (unsigned long)(hpi10 / 10u), (unsigned long)(hpi10 % 10u),
                   (unsigned long)(gpi100 / 100u), (unsigned long)(gpi100 % 100u),
                   (unsigned long long)insns,
                   (unsigned long)(guest * 1000000u / (now - hb_us + 1u)));
            hb_insns = g_atom.instructions;
            run_us = 0;
            /* The deck: where the tape is, and how many of these fields
             * ran unpaced. rt above is the turbo factor while it plays. */
            if (g_atom.cas.loaded) {
                const cassette_t *cas = &g_atom.cas;
                log_printf("  cassette     : %s %u%%, %lu edges, %lu turbo fields | %s, "
                           "%lu bytes recorded, %lu unframed, image %lu of %lu bytes%s\n",
                           cas->ended ? "end" : cas->playing ? "playing" : "stopped",
                           cassette_percent(cas), (unsigned long)cas->edges,
                           (unsigned long)turbo_fields,
                           cas->rec.on ? (cas->rec.full ? "recording, full" : "recording")
                                       : cas->wbuf ? "writable" : "protected",
                           (unsigned long)cas->rec.bytes, (unsigned long)cas->rec.errors,
                           (unsigned long)cas->uef.len, (unsigned long)cas->cap,
                           cas->dirty ? ", not on the card" : "");
            }
            turbo_fields = 0;
#if PICO_ATOM_AUDIO
            /* The consumed-sample rate against the microsecond timer is
             * the control quantity: it is the PWM wrap, measured, and it
             * must not move whatever the guest does. */
            audio_stats_t au;
            audio_stats(&au, true);
            uint32_t rate = (uint32_t)((uint64_t)(au.consumed - au_last.consumed) *
                                       1000000u / (now - hb_us + 1u));
            log_printf("  audio        : %lu Hz consumed, queue %lu (low %lu), "
                   "underrun samples %lu, late refills %lu, core overflow %lu, "
                   "speaker edges %lu, log dropped %u%s\n",
                   (unsigned long)rate, (unsigned long)au.level,
                   (unsigned long)au.low_water,
                   (unsigned long)au.underrun_samples, (unsigned long)au.late_refills,
                   (unsigned long)g_atom.beeper.overflow,
                   (unsigned long)g_atom.beeper.edges, log_dropped(),
                   au.started ? "" : " (not started)");
            au_last = au;
#else
            log_printf("  pacing       : %lu late fields\n", (unsigned long)late);
#endif
            hb_us = now;
            hb_cycles = g_atom.cpu.cycles;
        }
    }
}
