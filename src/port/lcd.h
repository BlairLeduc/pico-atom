/* lcd.h — ST7789P/ST7365P-class 320x320 panel on spi1 (design.md §8;
 * hardware-notes.md §4).
 *
 * Owned by core 1 once it is running, and only ever touched from thread
 * context — never from an interrupt handler (hardware-notes.md §5.7).
 * Nothing here masks interrupts: the controller tolerates SPI clock
 * pauses mid-window, so the audio refill IRQ may preempt a blit freely.
 */
#ifndef PICO_ATOM_LCD_H
#define PICO_ATOM_LCD_H

#include <stdbool.h>
#include <stdint.h>

/* Reset, initialise, clear frame memory, then DISPON (hardware-notes.md
 * §4.4, §10). Call after board_init_clocks(): the baud rate is derived
 * from clk_peri. Returns the SPI rate actually configured, which is a
 * configured figure, not one measured on SCK (§4.2). */
uint32_t lcd_init(void);

/* Re-apply the SPI baud rate. Required after any clk_sys change, because
 * the divider is computed from clk_peri at the time it is set (§3). */
uint32_t lcd_reapply_baud(void);

/* Fill a rectangle with one colour, by DMA with the read address held.
 * Blocks until the wire is idle. */
void lcd_fill(unsigned x, unsigned y, unsigned w, unsigned h, uint16_t rgb565);

/* The ping-pong pattern of hardware-notes.md §4.6(b):
 *
 *     lcd_blit_begin(x, y, w, h);
 *     for each row: fill a line buffer, lcd_blit_row(buf, w);
 *     lcd_blit_end();
 *
 * lcd_blit_row waits for the previous row's DMA to finish before starting
 * this one, so with two line buffers the caller always fills the one that
 * is not on the wire. Sending the same buffer twice in a row is allowed —
 * that is how vertical stretch costs nothing (design.md §8.3). */
void lcd_blit_begin(unsigned x, unsigned y, unsigned w, unsigned h);
void lcd_blit_row(const uint16_t *px, unsigned n);
void lcd_blit_end(void);

#endif /* PICO_ATOM_LCD_H */
