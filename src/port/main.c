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

#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "pico/multicore.h"
#include "pico/stdlib.h"

#include "atom.h"
#include "audio.h"
#include "board.h"
#include "display.h"
#include "hot.h"
#include "kbd.h"
#include "keymapio.h"
#include "keymatrix.h"
#include "lcd.h"
#include "log.h"
#include "mc6847.h"
#include "menu.h"
#include "roms.h"
#include "snappool.h"
#include "southbridge.h"
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
} g_c1;

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

static volatile uint32_t g_handoff = HANDOFF_NONE;

/* Written by the menu on core 1 while core 0 is parked, applied by core
 * 0 once it has the machine back. */
static menu_settings_t g_settings = { .volume = 8 };

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

    /*    The card's game keymaps too, so that the first tape load can
     *    choose one (design.md §10.5). The menu reads them again when it
     *    opens. */
    if (ok && storage_mount() == 0) {
        keymapio_scan();
#ifdef PICO_ATOM_BOOT_TAPE
        /* In the deck, stopped, for a run driven over the UART, where
         * the menu cannot be reached. */
        const char *err = tapeio_insert(&g_atom, PICO_ATOM_BOOT_TAPE);
        printf("  tape         : boot tape %s: %s\n", PICO_ATOM_BOOT_TAPE,
               err ? err : "in the deck");
#endif
        storage_unmount();
    }
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

        log_pump();

        if (g_handoff != HANDOFF_NONE) {
            __dmb();
            if (g_handoff == HANDOFF_TAPE) {
                char loaded[ATOM_ATM_NAME_LEN + 1];
                if (tapeio_serve(&g_atom, loaded)) keys_for_tape(loaded);
            } else {
                menu_run(&g_atom, &g_settings, s_scene);
            }
            __dmb();
            g_handoff = HANDOFF_NONE;
        }

        if (time_us_32() - last_poll >= KBD_POLL_US) {
            last_poll = time_us_32();
            g_c1.key_events += kbd_poll();
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
    bool shifted = c != 0 && strchr("!\"#$%&'()=<+*>?", c) != NULL;
    if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + 32u);   /* unshifted = capitals */
    else if (c >= 'a' && c <= 'z') { c = (uint8_t)(c - 32u); shifted = true; }
    if (shifted) keymatrix_event(&g_keys, KEY_EV_PRESSED, PICOCALC_KEY_SHIFT_L);
    keymatrix_event(&g_keys, KEY_EV_PRESSED, c);
    if (shifted) keymatrix_event(&g_keys, KEY_EV_RELEASED, PICOCALC_KEY_SHIFT_L);
    keymatrix_event(&g_keys, KEY_EV_RELEASED, keymap_picocalc_canonical(c));
}
#endif

/* Hand the machine to core 1 and wait for it back (g_handoff). */
static void park(uint32_t why) {
    __dmb();
    g_handoff = why;
#if PICO_ATOM_AUDIO
    static const int16_t silence[128];
    while (g_handoff != HANDOFF_NONE) audio_push(silence, 128u);
#else
    while (g_handoff != HANDOFF_NONE) sleep_us(100);
#endif
    __dmb();
}

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
#else
    printf("  audio        : disabled; pacing on the microsecond timer\n");
#endif

    atom_reset(&g_atom);   /* the vectors arrived with the kernel */
    printf("  guest        : started at #%04X; hot code in SRAM to tier %u "
           "(hot.h)\n", g_atom.cpu.pc, (unsigned)PICO_ATOM_RAM_TIER);

    uint32_t field = 0;
#if PICO_ATOM_AUDIO
    static int16_t pcm[ATOM_AUDIO_BUF_LEN];
    audio_stats_t au_last = { 0 };
#else
    uint32_t late = 0;
    const uint32_t field_us = 1000000u / g_atom.cfg.field_hz;
    absolute_time_t next = get_absolute_time();
#endif
    uint64_t hb_us = time_us_64(), hb_cycles = g_atom.cpu.cycles;
    uint64_t hb_insns = g_atom.instructions;
    uint32_t run_us = 0;   /* inside atom_run_field since the heartbeat */
    uint32_t turbo_fields = 0;
    const uint32_t clk_mhz = clock_get_hz(clk_sys) / 1000000u;
    for (;;) {
#if !PICO_ATOM_AUDIO
        /* Absolute, not incremental, so a late field does not accumulate
         * (§12.2). Turbo runs unpaced and restarts the schedule after. */
        if (PICO_ATOM_TURBO && atom_cassette_playing(&g_atom)) {
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
        if (g_keys.menu_request) {
            /* The menu pauses the guest (§13). Its keys, the chord's own
             * releases among them, are core 1's while it is open, so the
             * held set starts again empty. */
            park(HANDOFF_MENU);
            keymatrix_init(&g_keys);
            keymatrix_set_layout(&g_keys, g_settings.layout);
#if PICO_ATOM_AUDIO
            audio_set_volume(g_settings.volume * 32u);
#endif
        }

        uint32_t t0 = time_us_32();
        atom_run_field(&g_atom);
        run_us += time_us_32() - t0;

        int i = pool_claim();
        if (i >= 0) {
            snapshot_t *s = &g_pool.buf[i];
            memcpy(s->vram, atom_vram(&g_atom), ATOM_VRAM_SIZE);
            s->mode = atom_vdg_mode(&g_atom);
            s->field = field;
            pool_publish(i);
        }

        bool turbo = PICO_ATOM_TURBO && atom_cassette_playing(&g_atom);
        if (turbo) turbo_fields++;
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

        if (++field % (g_atom.cfg.field_hz * 5u) == 0) {
            /* Real-time ratio, guest seconds per wall second, in
             * thousandths: the headline number (§12.3). */
            uint64_t now = time_us_64();
            uint64_t guest = g_atom.cpu.cycles - hb_cycles;
            uint32_t rt1000 = (uint32_t)(guest * 1000u * 1000000u / ATOM_CPU_HZ /
                                         (now - hb_us + 1u));
            const mc6847_mode_info_t *vdg = mc6847_mode_info(atom_vdg_mode(&g_atom));
            log_printf("  heartbeat    : %lu fields, rt %lu.%03lu, %llu guest cycles, "
                   "%u undoc op(s), VDG %s | %s %lu presents (%lu full, "
                   "%lu dropped), last %lu us, max %lu us, i2c errors %lu | "
                   "keys %lu (%lu lost), tape calls %lu\n",
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
                   (unsigned long)g_atom.tape.served);
            /* Where core 0's time goes (§12.3, design.md §6.3). The
             * guest is paced, so rt above reads 1.000 whatever the code
             * costs; the cost is the time spent inside the guest.
             * Headroom is guest cycles per microsecond of it — how many
             * times real time the guest would run unpaced. */
            uint64_t insns = g_atom.instructions - hb_insns;
            uint32_t busy1000 = (uint32_t)((uint64_t)run_us * 1000u / (now - hb_us + 1u));
            uint32_t head100 = (uint32_t)(guest * 100u / (run_us + 1u));
            uint32_t hpi10 = (uint32_t)((uint64_t)run_us * clk_mhz * 10u / (insns + 1u));
            uint32_t gpi100 = (uint32_t)(guest * 100u / (insns + 1u));
            log_printf("  perf         : tier %u, guest %lu.%lu%% of wall, headroom "
                   "%lu.%02lux, %lu.%lu host cycles/insn, %lu.%02lu guest cycles/insn, "
                   "%llu insns\n",
                   (unsigned)PICO_ATOM_RAM_TIER,
                   (unsigned long)(busy1000 / 10u), (unsigned long)(busy1000 % 10u),
                   (unsigned long)(head100 / 100u), (unsigned long)(head100 % 100u),
                   (unsigned long)(hpi10 / 10u), (unsigned long)(hpi10 % 10u),
                   (unsigned long)(gpi100 / 100u), (unsigned long)(gpi100 % 100u),
                   (unsigned long long)insns);
            hb_insns = g_atom.instructions;
            run_us = 0;
            /* The deck: where the tape is, and how many of these fields
             * ran unpaced. rt above is the turbo factor while it plays. */
            if (g_atom.cas.loaded) {
                log_printf("  cassette     : %s %u%%, %lu edges, %lu turbo fields\n",
                           g_atom.cas.ended ? "end" : g_atom.cas.playing ? "playing" : "stopped",
                           cassette_percent(&g_atom.cas), (unsigned long)g_atom.cas.edges,
                           (unsigned long)turbo_fields);
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
