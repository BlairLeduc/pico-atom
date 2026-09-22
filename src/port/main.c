/* main.c — bring-up (design.md §17 M0; order per hardware-notes.md §10).
 *
 * M0 is the skeleton: clocks up, UART1 talking, the portable core linked
 * in and demonstrably running. The peripherals arrive in order at M3
 * (southbridge I2C, LCD), M4 (keyboard, SD) and M5 (audio); each is a
 * shippable milestone with its own smoke test before the next starts.
 */

#include <stdio.h>

#include "pico/stdlib.h"

#include "atom.h"
#include "board.h"
#include "mc6847.h"

#if PICO_ATOM_HAVE_FONT
#include "mc6847_font.h"
#endif

/* The guest lives in .bss, not the heap: src/core/ has no allocator, and
 * keeping it static is what makes the §5 budget a link-time fact. */
static atom_t g_atom;

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
#if PICO_ATOM_HAVE_FONT
    mc6847_set_font(&g_atom.vdg, font_6847);
#endif
    printf("  guest        : %u cycles/field at %u Hz, %u KiB address space\n",
           (unsigned)atom_cycles_per_field(&g_atom), g_atom.cfg.field_hz,
           (unsigned)(ATOM_ADDR_SPACE / 1024u));
    if (!mc6847_has_font(&g_atom.vdg)) {
        printf("  WARNING: no MC6847 character ROM supplied; alpha mode will "
               "draw placeholder cells (design.md §16)\n");
    }

    /* No ROMs yet: they come off the SD card at M4, and the build ships
     * none (design.md §1, §11.1). Until then the machine has nothing to
     * execute, so M0 just proves the core links, runs and paces. */
    uint32_t field = 0;
    absolute_time_t next = get_absolute_time();
    for (;;) {
        next = delayed_by_us(next, 1000000u / g_atom.cfg.field_hz);
        sleep_until(next);

        g_atom.budget += (int32_t)atom_cycles_per_field(&g_atom);
        if (g_atom.budget > 0) {
            g_atom.budget -= (int32_t)atom_run(&g_atom, (uint32_t)g_atom.budget);
        }

        /* FS, port C bit 7: low for the flyback interval, which is about
         * 6 % of a field (§12.1). Atom programs poll it to avoid writing
         * VRAM during active display. */
        atom_field_sync(&g_atom, true);
        atom_field_sync(&g_atom, false);

        if (++field % (g_atom.cfg.field_hz * 5u) == 0) {
            const mc6847_mode_info_t *vdg = mc6847_mode_info(atom_vdg_mode(&g_atom));
            printf("  heartbeat    : %lu fields, %llu guest cycles, "
                   "%u undocumented opcode(s), VDG %s\n",
                   (unsigned long)field,
                   (unsigned long long)g_atom.cpu.cycles,
                   (unsigned)g_atom.cpu.undoc_count,
                   vdg->name);
        }
    }
}
