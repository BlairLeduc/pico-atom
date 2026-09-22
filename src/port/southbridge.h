/* southbridge.h — the PicoCalc's keyboard/power MCU on i2c1 (hardware-notes.md §6).
 *
 * An STM32 at 0x1F, 10 kHz. Keyboard, both backlights, battery and power
 * all live behind it; nothing is wired to the processor directly. Each
 * transaction costs 4-5 ms of wall time (§6.1), which makes this the most
 * expensive routine operation on the machine — so it is polled from the
 * frame loop in thread context, never from a timer IRQ.
 *
 * This is the register layer only. Key-event normalisation belongs to
 * kbd.c (M4), on top of it.
 */
#ifndef PICO_ATOM_SOUTHBRIDGE_H
#define PICO_ATOM_SOUTHBRIDGE_H

#include <stdbool.h>
#include <stdint.h>

/* Registers (hardware-notes.md §6). */
#define SB_REG_VER  0x01u   /* [0, version]                                  */
#define SB_REG_KEY  0x04u   /* [FIFO depth | caps bit 5 | num bit 6, 0]      */
#define SB_REG_BKL  0x05u   /* [reg, LCD backlight] — steps of 16, 16..240   */
#define SB_REG_RST  0x08u   /* a READ resets the MCU; sb_read refuses it     */
#define SB_REG_FIF  0x09u   /* [state, key], [0,0] when empty                */
#define SB_REG_BK2  0x0Au   /* [reg, keyboard backlight]                     */
#define SB_REG_BAT  0x0Bu   /* [reg, percent | bit 7 charging]               */

#define SB_WRITE    0x80u   /* OR into the register number to write it       */

typedef enum {
    SB_OK      = 0,
    SB_BUSY    = -1,   /* another transaction is in flight            */
    SB_NACK    = -2,   /* no device answered, or the write failed     */
    SB_TIMEOUT = -3,   /* a wedged bus must not hang the machine      */
    SB_REFUSED = -4,   /* a register this driver will not touch       */
} sb_status_t;

/* i2c1 on GP6 (SDA) / GP7 (SCL) at 10 kHz, with pull-ups. Returns the
 * rate the peripheral actually took. */
uint32_t sb_init(void);

/* Every register replies with two bytes; what they mean is per register
 * (see above), so the raw pair is returned. */
sb_status_t sb_read(uint8_t reg, uint8_t reply[2]);

/* Write one register. The MCU prepares a two-byte reply to a write as
 * well; it is read back and returned in `reply` (may be NULL). */
sb_status_t sb_write(uint8_t reg, uint8_t value, uint8_t reply[2]);

/* Transactions that failed, of any kind. §12.3 wants this at zero. */
uint32_t sb_error_count(void);

#endif /* PICO_ATOM_SOUTHBRIDGE_H */
