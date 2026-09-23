/* main.c — bring-up and the two cores' loops (design.md §4.2, §12.1, §17).
 *
 * Core 0 owns the 6502 and runs it in field-sized slices, split at the
 * flyback boundary, publishing a VRAM snapshot per field. Core 1 owns
 * the southbridge, the LCD and the SD card: it brings them up in
 * hardware-notes.md §10's order, loads the ROMs, and then presents
 * snapshots and polls the keyboard.
 *
 * Audio (M5) is still to come. Until it exists the field loop paces on
 * time_us_64() against an absolute deadline — §12.2's fallback path.
 */

#include <stdio.h>
#include <string.h>

#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "atom.h"
#include "board.h"
#include "display.h"
#include "kbd.h"
#include "keymatrix.h"
#include "lcd.h"
#include "mc6847.h"
#include "roms.h"
#include "snappool.h"
#include "southbridge.h"

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
} g_c1;

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

    /* Alpha: a page of every glyph, some inverse, some SG4. */
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

/* No ROMs: show why, and keep the southbridge awake, for ever. A blank
 * screen is the most expensive failure to debug on this hardware
 * (design.md §11.1). The menu will offer a way out at M6. */
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
#if PICO_ATOM_MEASURE_PRESENT
    display_test_pattern();
    printf("  M3 pattern   : white border on (32,64)-(287,255); corners "
           "red TL, green TR, blue BL, yellow BR; outside black\n");
    sleep_ms(5000);
    measure_present();
#endif
    display_invalidate();

    /* 3. The card, while core 0 waits: loading writes the ROMs into the
     *    machine, the one time core 1 touches atom_t (roms.h). */
    roms_report_t report;
    bool ok = roms_load(&g_atom, &report);
    roms_log(&report);
    g_c1.roms_ok = ok;
    __dmb();
    g_c1.ready = true;
    if (!ok) no_roms(&report);

    /* Live: present the newest snapshot, drop superseded ones (§12.2),
     * and poll the keyboard at 30 Hz. */
    uint32_t last_poll = time_us_32();
    for (;;) {
        int i = pool_take();
        if (i >= 0) {
            display_stats_t st;
            display_present(g_pool.buf[i].vram, g_pool.buf[i].mode, &st);
            pool_release(i);

            g_c1.presents++;
            if (st.full) g_c1.full_presents++;
            g_c1.last_us = st.us;
            if (st.us > g_c1.max_us) g_c1.max_us = st.us;
        }

        if (time_us_32() - last_poll >= KBD_POLL_US) {
            last_poll = time_us_32();
            g_c1.key_events += kbd_poll();
        }

        /* A hardware-timer wait between iterations rather than spinning
         * on shared state (design.md §4.2). */
        if (i < 0) sleep_us(20);
    }
}

/* ---- core 0: the guest ------------------------------------------------- */

int main(void) {
    stdio_init_all();

    bool clocks_ok = board_init_clocks();

    board_info_t info;
    board_identify(&info);

    /* A startup banner plus consecutive heartbeats is more useful boot
     * evidence than a single line (hardware-notes.md §2.7). */
    board_log_banner(&info);
    if (!clocks_ok) {
        printf("  WARNING: clk_sys is not at 150 MHz; SPI and audio rates "
               "will not be the ones this build assumes\n");
    }

    atom_config_t cfg;
    atom_config_default(&cfg);
    atom_init(&g_atom, &cfg);
    printf("  guest        : %u cycles/field at %u Hz, flyback %u, %u KiB address space\n",
           (unsigned)atom_cycles_per_field(&g_atom), g_atom.cfg.field_hz,
           (unsigned)atom_flyback_cycles(&g_atom),
           (unsigned)(ATOM_ADDR_SPACE / 1024u));
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
    if (!g_c1.roms_ok) {
        printf("  guest        : not started — no ROMs (the panel says which)\n");
        for (;;) sleep_ms(1000);
    }
    atom_reset(&g_atom);   /* the vectors arrived with the kernel */
    printf("  guest        : started at #%04X\n", g_atom.cpu.pc);

    uint32_t field = 0, late = 0;
    const uint32_t field_us = 1000000u / g_atom.cfg.field_hz;
    absolute_time_t next = get_absolute_time();
    for (;;) {
        /* Absolute, not incremental, so a late field does not accumulate
         * (§12.2). */
        next = delayed_by_us(next, field_us);
        if (absolute_time_diff_us(get_absolute_time(), next) < 0) late++;
        sleep_until(next);

        /* Keys first, so the matrix the guest scans this field is the
         * one the events describe (§10.2). */
        uint8_t kstate, kcode;
        while (kbd_pop(&kstate, &kcode)) keymatrix_event(&g_keys, kstate, kcode);
        keymatrix_field(&g_keys, &g_atom);
        if (g_keys.menu_request) {
            g_keys.menu_request = false;
            printf("  menu         : Alt+M — the menu arrives at M6 (design.md §13)\n");
        }

        atom_run_field(&g_atom);

        int i = pool_claim();
        if (i >= 0) {
            snapshot_t *s = &g_pool.buf[i];
            memcpy(s->vram, atom_vram(&g_atom), ATOM_VRAM_SIZE);
            s->mode = atom_vdg_mode(&g_atom);
            s->field = field;
            pool_publish(i);
        }

        if (++field % (g_atom.cfg.field_hz * 5u) == 0) {
            const mc6847_mode_info_t *vdg = mc6847_mode_info(atom_vdg_mode(&g_atom));
            printf("  heartbeat    : %lu fields (%lu late), %llu guest cycles, "
                   "%u undoc op(s), VDG %s | %s %lu presents (%lu full, "
                   "%lu dropped), last %lu us, max %lu us, i2c errors %lu | "
                   "keys %lu (%lu lost)\n",
                   (unsigned long)field, (unsigned long)late,
                   (unsigned long long)g_atom.cpu.cycles,
                   (unsigned)g_atom.cpu.undoc_count, vdg->name,
                   g_c1.ready ? "live" : "bring-up",
                   (unsigned long)g_c1.presents, (unsigned long)g_c1.full_presents,
                   (unsigned long)g_pool.dropped,
                   (unsigned long)g_c1.last_us, (unsigned long)g_c1.max_us,
                   (unsigned long)sb_error_count(),
                   (unsigned long)g_c1.key_events,
                   (unsigned long)(kbd_overflows() + g_keys.dropped));
        }
    }
}
