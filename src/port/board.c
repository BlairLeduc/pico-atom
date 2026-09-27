/* board.c — clocks and board identification (design.md §3.1-3.2). */

#include "board.h"

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "hardware/clocks.h"
#include "hardware/sync.h"
#include "hardware/vreg.h"
#include "pico/stdlib.h"
#include "pico/unique_id.h"

#if PICO_RP2350
#include "hardware/structs/qmi.h"
#include "hardware/structs/sysinfo.h"
#endif

/* Ship at the stock 150 MHz. That is the rated part speed and one of only
 * two points where the SPI divider delivers the full 75 MHz to the panel
 * (hardware-notes.md §3). 200 MHz would make the 6502 1.33x faster, which
 * we do not need, and the display 1.5x slower, which we cannot afford.
 * 300 MHz is the other point, and the settings file may ask for it
 * (design.md §3.2). */
#define BOARD_MHZ      150u
#define BOARD_MHZ_FAST 300u

#if PICO_RP2350
/* Flash XIP runs off clk_sys through the QMI's divider, and the bootrom
 * chose it for 150 MHz. Scale it, and the RX delay, which is in half
 * clk_sys cycles, by the same factor, so the flash sees at 300 MHz
 * exactly the timing it had at 150 (hardware-notes.md §3). The hot code
 * is in SRAM already (hot.h), so it is only cold code that does not get
 * faster. From SRAM, with interrupts off: nothing may fetch from flash
 * while its timing changes. */
static void __no_inline_not_in_flash_func(flash_timing_scale)(unsigned mul) {
    uint32_t irq = save_and_disable_interrupts();
    uint32_t t = qmi_hw->m[0].timing;
    uint32_t div = (t & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
    uint32_t rx = (t & QMI_M0_TIMING_RXDELAY_BITS) >> QMI_M0_TIMING_RXDELAY_LSB;
    div *= mul;
    rx *= mul;
    if (div > 255u) div = 255u;
    if (rx > 7u) rx = 7u;
    t &= ~(QMI_M0_TIMING_CLKDIV_BITS | QMI_M0_TIMING_RXDELAY_BITS);
    t |= div << QMI_M0_TIMING_CLKDIV_LSB | rx << QMI_M0_TIMING_RXDELAY_LSB;
    qmi_hw->m[0].timing = t;
    /* The write lands before anything after it is fetched from flash. */
    __dsb();
    __isb();
    restore_interrupts(irq);
}
#endif

bool board_init_clocks(unsigned mhz) {
    if (mhz != BOARD_MHZ_FAST) mhz = BOARD_MHZ;
    if (clock_get_hz(clk_sys) == mhz * 1000000u) {
        /* Already there; still re-apply clk_peri below. */
    } else if (mhz == BOARD_MHZ_FAST) {
#if PICO_RP2350
        /* Only up from 150, at power-on, before anything else is running.
         * The rail goes up first and settles, so it is never the lower of
         * the two while the clock is the higher (hardware-notes.md §3). */
        if (clock_get_hz(clk_sys) != BOARD_MHZ * 1000000u &&
            !set_sys_clock_khz(BOARD_MHZ * 1000u, false)) {
            return false;
        }
        vreg_set_voltage(VREG_VOLTAGE_1_20);
        busy_wait_us_32(1000);
        flash_timing_scale(BOARD_MHZ_FAST / BOARD_MHZ);
        if (!set_sys_clock_khz(BOARD_MHZ_FAST * 1000u, false)) {
            /* The flash stays slower than it need be; the rail stays up.
             * Neither is a fault, and the caller says the clock failed. */
            return false;
        }
#else
        return false;   /* RP2040: 300 MHz is well past its rating */
#endif
    } else if (!set_sys_clock_khz(mhz * 1000u, false)) {
        return false;
    }
#if PICO_RP2350
    /* The rail is not reset with the chip: after a run at 300 MHz, a
     * watchdog or SWD reset comes back to 150 still at 1.20 V. Put it back
     * once the clock is down, never before (hardware-notes.md §3). The
     * flash's timing needs nothing: the bootrom sets it again. */
    if (mhz == BOARD_MHZ) vreg_set_voltage(VREG_VOLTAGE_DEFAULT);
#endif

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
#if PICO_RP2350
    info->qmi_timing  = qmi_hw->m[0].timing;
    info->vreg_mv     = 550u + 50u * (unsigned)vreg_get_voltage();
#endif
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
#if PICO_RP2350
    unsigned div = (info->qmi_timing & QMI_M0_TIMING_CLKDIV_BITS) >> QMI_M0_TIMING_CLKDIV_LSB;
    printf("  flash, core  : QMI clkdiv %u rxdelay %lu (%lu kHz), core rail ~%u mV\n", div,
           (unsigned long)((info->qmi_timing & QMI_M0_TIMING_RXDELAY_BITS) >>
                           QMI_M0_TIMING_RXDELAY_LSB),
           (unsigned long)(div ? info->clk_sys_hz / div / 1000u : 0u), info->vreg_mv);
#endif
}
