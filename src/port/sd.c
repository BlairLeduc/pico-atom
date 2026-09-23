/* sd.c — SD card in SPI mode on spi0 (hardware-notes.md §7.1).
 *
 * The SD Physical Layer Simplified Specification's SPI-mode sequence:
 * CMD0, CMD8 to tell a v2 card from a v1, ACMD41 until ready, CMD58 for
 * the capacity bit, CMD16 on byte-addressed cards. Single-block reads
 * and writes only; nothing here needs more.
 */

#include "sd.h"

#include "hardware/gpio.h"
#include "hardware/spi.h"
#include "pico/stdlib.h"

#define SD_SPI      spi0
#define SD_PIN_MISO 16
#define SD_PIN_CS   17
#define SD_PIN_SCK  18
#define SD_PIN_MOSI 19
#define SD_PIN_CD   22

#define SD_INIT_HZ  400000u     /* §7.1 */
#define SD_RUN_HZ   25000000u   /* §7.1: requested, not measured */

/* Bounds, all generous against the specification's own limits. */
#define SD_READY_US     500000u  /* card busy before a command, or after a write */
#define SD_TOKEN_US     200000u  /* data token after CMD17 (spec: 100 ms)       */
#define SD_ACMD41_US   1000000u  /* initialisation (spec: 1 s)                  */

#define R1_IDLE     0x01u
#define R1_ILLEGAL  0x04u

#define TOKEN_START 0xFEu

static bool s_hc;   /* block addressing */

static uint8_t xfer(uint8_t out) {
    uint8_t in;
    spi_write_read_blocking(SD_SPI, &out, &in, 1);
    return in;
}

static void select(void)   { gpio_put(SD_PIN_CS, 0); }

static void deselect(void) {
    gpio_put(SD_PIN_CS, 1);
    xfer(0xFF);    /* one more clock byte so the card releases MISO */
}

/* The card holds MISO low while busy. */
static bool wait_ready(uint32_t us) {
    absolute_time_t until = make_timeout_time_ms(us / 1000u);
    do {
        if (xfer(0xFF) == 0xFF) return true;
    } while (absolute_time_diff_us(get_absolute_time(), until) > 0);
    return false;
}

/* Send a command and return R1, or 0xFF if nothing answered. Leaves the
 * card selected: the caller reads any trailing bytes and deselects. */
static uint8_t command(uint8_t cmd, uint32_t arg) {
    select();
    if (cmd != 0 && !wait_ready(SD_READY_US)) return 0xFF;

    /* CRC is checked only for CMD0 and CMD8 in SPI mode; these are the
     * specification's fixed values for their fixed arguments. */
    uint8_t crc = (cmd == 0) ? 0x95u : (cmd == 8) ? 0x87u : 0x01u;
    uint8_t frame[6] = {
        (uint8_t)(0x40u | cmd),
        (uint8_t)(arg >> 24), (uint8_t)(arg >> 16), (uint8_t)(arg >> 8), (uint8_t)arg,
        crc,
    };
    spi_write_blocking(SD_SPI, frame, sizeof frame);

    /* R1 arrives within eight bytes; its bit 7 is always clear. */
    for (int i = 0; i < 8; i++) {
        uint8_t r1 = xfer(0xFF);
        if (!(r1 & 0x80u)) return r1;
    }
    return 0xFF;
}

static uint8_t app_command(uint8_t cmd, uint32_t arg) {
    uint8_t r1 = command(55, 0);
    deselect();
    if (r1 > R1_IDLE) return r1;
    return command(cmd, arg);
}

bool sd_present(void) {
    return !gpio_get(SD_PIN_CD);
}

static void pins_init(void) {
    static bool done;
    if (done) return;
    done = true;

    spi_init(SD_SPI, SD_INIT_HZ);
    spi_set_format(SD_SPI, 8, SPI_CPOL_0, SPI_CPHA_0, SPI_MSB_FIRST);
    gpio_set_function(SD_PIN_MISO, GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_SCK,  GPIO_FUNC_SPI);
    gpio_set_function(SD_PIN_MOSI, GPIO_FUNC_SPI);
    gpio_pull_up(SD_PIN_MISO);     /* an absent card must read 0xFF */

    gpio_init(SD_PIN_CS);
    gpio_put(SD_PIN_CS, 1);
    gpio_set_dir(SD_PIN_CS, GPIO_OUT);

    gpio_init(SD_PIN_CD);
    gpio_set_dir(SD_PIN_CD, GPIO_IN);
    gpio_pull_up(SD_PIN_CD);       /* §7.1: active low, needs the pull-up */
    sleep_us(10);                  /* let the pull-up charge the line */
}

sd_status_t sd_init(uint32_t *hz, bool *high_capacity) {
    pins_init();
    if (!sd_present()) return SD_NO_CARD;

    /* Clock rate is re-applied every time: clk_peri may have moved
     * (hardware-notes.md §3). */
    spi_set_baudrate(SD_SPI, SD_INIT_HZ);

    /* At least 74 clocks with CS and MOSI high puts the card in its
     * native mode, ready for CMD0 to select SPI. */
    gpio_put(SD_PIN_CS, 1);
    for (int i = 0; i < 10; i++) xfer(0xFF);

    uint8_t r1 = 0xFF;
    for (int tries = 0; tries < 10 && r1 != R1_IDLE; tries++) {
        r1 = command(0, 0);
        deselect();
    }
    if (r1 != R1_IDLE) return SD_NO_REPLY;

    /* CMD8: a v2 card echoes the check pattern; a v1 card calls it
     * illegal. */
    bool v2 = false;
    r1 = command(8, 0x1AAu);
    if (r1 == R1_IDLE) {
        uint8_t r7[4];
        for (int i = 0; i < 4; i++) r7[i] = xfer(0xFF);
        deselect();
        if ((r7[2] & 0x0Fu) != 0x01u || r7[3] != 0xAAu) return SD_UNUSABLE;
        v2 = true;
    } else {
        deselect();
        if (!(r1 & R1_ILLEGAL)) return SD_REJECTED;
    }

    /* ACMD41 until the card leaves idle. HCS says we can take SDHC. */
    absolute_time_t until = make_timeout_time_ms(SD_ACMD41_US / 1000u);
    do {
        r1 = app_command(41, v2 ? 0x40000000u : 0);
        deselect();
        if (r1 == 0) break;
        if (r1 != R1_IDLE) return SD_REJECTED;   /* includes MMC, not handled */
    } while (absolute_time_diff_us(get_absolute_time(), until) > 0);
    if (r1 != 0) return SD_NO_REPLY;

    s_hc = false;
    if (v2) {
        /* CMD58: OCR bit 30 is CCS, block addressing. */
        r1 = command(58, 0);
        uint8_t ocr[4];
        for (int i = 0; i < 4; i++) ocr[i] = xfer(0xFF);
        deselect();
        if (r1 != 0) return SD_REJECTED;
        s_hc = (ocr[0] & 0x40u) != 0;
    }
    if (!s_hc) {
        /* Byte-addressed cards: make the block length the sector size. */
        r1 = command(16, SD_BLOCK);
        deselect();
        if (r1 != 0) return SD_REJECTED;
    }

    uint32_t got = spi_set_baudrate(SD_SPI, SD_RUN_HZ);
    if (hz) *hz = got;
    if (high_capacity) *high_capacity = s_hc;
    return SD_OK;
}

static uint32_t address(uint32_t block) {
    return s_hc ? block : block * SD_BLOCK;
}

sd_status_t sd_read(uint32_t block, uint8_t *buf) {
    uint8_t r1 = command(17, address(block));
    if (r1 != 0) {
        deselect();
        return r1 == 0xFF ? SD_NO_REPLY : SD_REJECTED;
    }

    absolute_time_t until = make_timeout_time_ms(SD_TOKEN_US / 1000u);
    uint8_t tok;
    do {
        tok = xfer(0xFF);
    } while (tok == 0xFF && absolute_time_diff_us(get_absolute_time(), until) > 0);
    if (tok != TOKEN_START) {
        deselect();
        return tok == 0xFF ? SD_NO_REPLY : SD_REJECTED;
    }

    spi_read_blocking(SD_SPI, 0xFF, buf, SD_BLOCK);
    xfer(0xFF);    /* CRC, not checked in SPI mode */
    xfer(0xFF);
    deselect();
    return SD_OK;
}

sd_status_t sd_write(uint32_t block, const uint8_t *buf) {
    uint8_t r1 = command(24, address(block));
    if (r1 != 0) {
        deselect();
        return r1 == 0xFF ? SD_NO_REPLY : SD_REJECTED;
    }

    xfer(0xFF);
    xfer(TOKEN_START);
    spi_write_blocking(SD_SPI, buf, SD_BLOCK);
    xfer(0xFF);    /* dummy CRC */
    xfer(0xFF);

    /* Data response: xxx0sss1, where sss = 010 is accepted. */
    uint8_t resp = xfer(0xFF);
    if ((resp & 0x1Fu) != 0x05u) {
        deselect();
        return SD_REJECTED;
    }
    bool ok = wait_ready(SD_READY_US);    /* programming */
    deselect();
    return ok ? SD_OK : SD_NO_REPLY;
}
