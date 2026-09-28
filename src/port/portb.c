/* portb.c — VIA port B on the GPIOs (portb.h, design.md §7.4). */

#include "portb.h"

#include <stdio.h>

#include "hardware/gpio.h"
#include "hardware/uart.h"

static struct {
    uint32_t mask[SETTINGS_PB_BITS];   /* PBi's GPIO as a bit, 0 for none */
    uint32_t held;                     /* every GPIO port B holds         */
    uint32_t was_up, was_down;         /* the pads' pulls before it did   */
} s;

/* The development build's UART1; the build that ships has none, and its
 * GP4 and GP5 are pins like the rest. */
#if PICO_ATOM_UART
#define UART_PINS ((1u << PICO_DEFAULT_UART_TX_PIN) | (1u << PICO_DEFAULT_UART_RX_PIN))
#else
#define UART_PINS 0u
#endif

/* Core 0's. What the chip drives goes out, the level first so that a pin
 * turning to an output does not glitch; the rest are inputs. */
static void pins(atom_t *m, bool write) {
    if (write) {
        uint8_t out = via6522_pb_out(&m->via);
        uint8_t dir = m->via.ddrb;
        if (m->via.acr & VIA_ACR_T1_PB7) dir |= 0x80u;
        uint32_t high = 0, oe = 0;
        for (unsigned i = 0; i < SETTINGS_PB_BITS; i++) {
            if (out & (1u << i)) high |= s.mask[i];
            if (dir & (1u << i)) oe |= s.mask[i];
        }
        gpio_put_masked(s.held, high);
        gpio_set_dir_masked(s.held, oe);
        return;
    }
    uint32_t level = gpio_get_all();
    uint8_t in = 0xFFu;
    for (unsigned i = 0; i < SETTINGS_PB_BITS; i++)
        if (s.mask[i] && !(level & s.mask[i])) in = (uint8_t)(in & ~(1u << i));
    via6522_set_pb(&m->via, in);
}

static void release(unsigned gp) {
    uint32_t bit = 1u << gp;
    gpio_set_dir(gp, GPIO_IN);
    gpio_set_pulls(gp, (s.was_up & bit) != 0, (s.was_down & bit) != 0);
    if (bit & UART_PINS) gpio_set_function(gp, UART_FUNCSEL_NUM(uart_default, gp));
    else gpio_deinit(gp);
}

/* An input with the pull-up, until DDRB says otherwise. */
static void claim(unsigned gp) {
    uint32_t bit = 1u << gp;
    if (gpio_is_pulled_up(gp)) s.was_up |= bit;
    if (gpio_is_pulled_down(gp)) s.was_down |= bit;
    gpio_init(gp);
    gpio_pull_up(gp);
}

void portb_set(atom_t *m, bool on, const uint8_t pin[SETTINGS_PB_BITS]) {
    for (unsigned gp = 0; gp < 32u; gp++)
        if (s.held & (1u << gp)) release(gp);
    s.held = s.was_up = s.was_down = 0;

    char line[80];
    int n = 0;
    for (unsigned i = 0; i < SETTINGS_PB_BITS; i++) {
        uint8_t gp = pin[i];
        bool ok = on && gp < 32u && (SETTINGS_PB_GPIOS & (1u << gp)) && !(s.held & (1u << gp));
        s.mask[i] = ok ? 1u << gp : 0u;
        s.held |= s.mask[i];
        if (ok) n += snprintf(line + n, sizeof line - (size_t)n, " PB%u=GP%u", i, gp);
    }
    /* Said before the UART's pins can go. */
    printf("  port b       : %s%s\n", s.held ? "on" : "off", s.held ? line : "");

    for (unsigned gp = 0; gp < 32u; gp++)
        if (s.held & (1u << gp)) claim(gp);
#if PICO_ATOM_UART
    if (s.held & (1u << PICO_DEFAULT_UART_RX_PIN))
        hw_clear_bits(&uart_get_hw(uart_default)->cr, UART_UARTCR_RXE_BITS);
    else
        hw_set_bits(&uart_get_hw(uart_default)->cr, UART_UARTCR_RXE_BITS);
#endif
    portb_attach(m);
}

void portb_attach(atom_t *m) {
    m->port_b = s.held ? pins : NULL;
    if (m->port_b) pins(m, true);
}

bool portb_has_uart(void) {
    return (s.held & UART_PINS) != 0;
}
