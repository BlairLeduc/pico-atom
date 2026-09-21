/* board.c — clocks and board identification (design.md §3.1-3.2). */

#include "board.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#if PICO_RP2350
#include "hardware/structs/sysinfo.h"
#endif

/* Ship at the stock 150 MHz. That is the rated part speed and one of only
 * two points where the SPI divider delivers the full 75 MHz to the panel
 * (hardware-notes.md §3). 200 MHz would make the 6502 1.33x faster, which
 * we do not need, and the display 1.5x slower, which we cannot afford. */
#define PICO_ATOM_CLK_SYS_HZ (150u * 1000u * 1000u)

bool board_init_clocks(void) {
    if (clock_get_hz(clk_sys) == PICO_ATOM_CLK_SYS_HZ) {
        /* Already there; still re-apply clk_peri below. */
    } else if (!set_sys_clock_khz(PICO_ATOM_CLK_SYS_HZ / 1000u, false)) {
        return false;
    }

    /* set_sys_clock_pll otherwise parks clk_peri on the 48 MHz USB PLL,
     * and clk_peri does not follow clk_sys by default. Every derived
     * clock — the SPI baud rate and the audio PWM carrier — is re-applied
     * by its own driver after this returns (hardware-notes.md §3). */
    clock_configure(clk_peri,
                    0,
                    CLOCKS_CLK_PERI_CTRL_AUXSRC_VALUE_CLK_SYS,
                    clock_get_hz(clk_sys),
                    clock_get_hz(clk_sys));
    return true;
}

void board_identify(board_info_t *info) {
    memset(info, 0, sizeof(*info));

    info->sdk_board = PICO_BOARD;
#if PICO_RP2350
    info->sdk_platform = "rp2350";
    info->chip_version = (uint8_t)((sysinfo_hw->chip_id >> 28) & 0xFu);
#elif PICO_RP2040
    info->sdk_platform = "rp2040";
    info->chip_version = 0;
#else
    info->sdk_platform = "unknown";
    info->chip_version = 0;
#endif

    pico_get_unique_board_id_string(info->unique_id, sizeof(info->unique_id));

    info->clk_sys_hz  = clock_get_hz(clk_sys);
    info->clk_peri_hz = clock_get_hz(clk_peri);
}

void board_log_banner(const board_info_t *info) {
    printf("\npico-atom — Acorn Atom for the PicoCalc\n");
    /* Build target and physical identity, reported separately and
     * labelled as such (hardware-notes.md §2.1). */
    printf("  build target : board=%s platform=%s\n",
           info->sdk_board, info->sdk_platform);
    printf("  module       : id=%s chip_rev=%u\n",
           info->unique_id, info->chip_version);
    printf("  clocks       : clk_sys=%lu Hz clk_peri=%lu Hz\n",
           (unsigned long)info->clk_sys_hz, (unsigned long)info->clk_peri_hz);
}
