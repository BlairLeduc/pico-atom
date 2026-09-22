/* southbridge.c — the PicoCalc's keyboard/power MCU on i2c1 (hardware-notes.md §6).
 *
 * Reply layout checked against the MCU firmware's receiveEvent dispatch,
 * Code/picocalc_keyboard/picocalc_keyboard.ino at clockworkpi/PicoCalc
 * f91519806d4b2e0a62c4638a9f695cd5162c5479.
 */

#include "southbridge.h"

#include "hardware/gpio.h"
#include "hardware/i2c.h"
#include "pico/stdlib.h"

#define SB_I2C      i2c1
#define SB_PIN_SDA  6
#define SB_PIN_SCL  7
#define SB_ADDR     0x1Fu
#define SB_HZ       10000u

/* A byte and its ACK are 0.9 ms at 10 kHz; each phase is at most three
 * bytes. 20 ms is generous without letting a wedged bus stall a frame
 * for long. */
#define SB_TIMEOUT_US 20000u

static volatile bool s_busy;
static uint32_t s_errors;

/* Guard the bus with a flag, so a foreground read and a background poll
 * cannot interleave (hardware-notes.md §6.1). It is a same-core guard:
 * the southbridge belongs to core 1 alone (design.md §4.2). */
static bool sb_acquire(void) {
    if (s_busy) return false;
    s_busy = true;
    return true;
}

static void sb_release(void) {
    s_busy = false;
}

static sb_status_t sb_status(int rc, int want) {
    if (rc == want) return SB_OK;
    s_errors++;
    return (rc == PICO_ERROR_TIMEOUT) ? SB_TIMEOUT : SB_NACK;
}

/* Write `n` bytes, then read the two-byte reply the MCU has prepared. */
static sb_status_t sb_transact(const uint8_t *out, size_t n, uint8_t reply[2]) {
    if (!sb_acquire()) return SB_BUSY;

    uint8_t scratch[2];
    uint8_t *in = reply ? reply : scratch;

    sb_status_t st = sb_status(
        i2c_write_timeout_us(SB_I2C, SB_ADDR, out, n, false, SB_TIMEOUT_US), (int)n);
    if (st == SB_OK) {
        st = sb_status(
            i2c_read_timeout_us(SB_I2C, SB_ADDR, in, 2, false, SB_TIMEOUT_US), 2);
    }

    sb_release();
    return st;
}

uint32_t sb_init(void) {
    uint32_t hz = i2c_init(SB_I2C, SB_HZ);
    gpio_set_function(SB_PIN_SDA, GPIO_FUNC_I2C);
    gpio_set_function(SB_PIN_SCL, GPIO_FUNC_I2C);
    gpio_pull_up(SB_PIN_SDA);
    gpio_pull_up(SB_PIN_SCL);
    return hz;
}

sb_status_t sb_read(uint8_t reg, uint8_t reply[2]) {
    /* A read of RST resets the MCU after one second (§6). Nothing in this
     * emulator wants that, and it is one typo away from SB_REG_FIF. */
    if ((reg & ~SB_WRITE) == SB_REG_RST) return SB_REFUSED;
    return sb_transact(&reg, 1, reply);
}

sb_status_t sb_write(uint8_t reg, uint8_t value, uint8_t reply[2]) {
    uint8_t out[2] = { (uint8_t)(reg | SB_WRITE), value };
    return sb_transact(out, 2, reply);
}

uint32_t sb_error_count(void) {
    return s_errors;
}
