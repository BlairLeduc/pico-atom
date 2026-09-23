/* sd.h — the SD card on spi0, in SPI mode (hardware-notes.md §7.1).
 *
 * Core 1 only, and only at a defined application boundary with the guest
 * paused: write latency can exceed both the field and the audio deadline
 * (design.md §11.1). Every wait is bounded; a card that stops answering
 * returns an error rather than hanging the machine.
 */
#ifndef PICO_ATOM_SD_H
#define PICO_ATOM_SD_H

#include <stdbool.h>
#include <stdint.h>

#define SD_BLOCK 512u

typedef enum {
    SD_OK       = 0,
    SD_NO_CARD  = -1,   /* card detect (GP22) says the slot is empty      */
    SD_NO_REPLY = -2,   /* a command or data token never came             */
    SD_REJECTED = -3,   /* the card answered with an error                */
    SD_UNUSABLE = -4,   /* voltage or version this driver does not handle */
} sd_status_t;

/* Is a card in the slot? GP22, active low, internal pull-up. */
bool sd_present(void);

/* Initialise at 400 kHz, then switch to the operating rate. On success
 * *hz is the rate configured — a configured figure, not one measured on
 * SCK — and *high_capacity says SDHC/SDXC block addressing. */
sd_status_t sd_init(uint32_t *hz, bool *high_capacity);

sd_status_t sd_read(uint32_t block, uint8_t *buf);
sd_status_t sd_write(uint32_t block, const uint8_t *buf);

#endif /* PICO_ATOM_SD_H */
