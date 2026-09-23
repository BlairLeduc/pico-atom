/* log.c — core 0's UART output, drained by core 1 (design.md §12.3). */

#include "log.h"

#include <stdarg.h>
#include <stdio.h>

#include "hardware/sync.h"
#include "hardware/uart.h"
#include "pico/stdlib.h"

/* Power of two, so the indices can run free and wrap by mask. */
#define LOG_RING 2048u

static char              s_ring[LOG_RING];
static volatile uint32_t s_head;       /* written by core 0 only */
static volatile uint32_t s_tail;       /* written by core 1 only */
static volatile unsigned s_dropped;

void log_printf(const char *fmt, ...) {
    char line[512];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    if (n <= 0) return;
    if ((unsigned)n >= sizeof line) n = sizeof line - 1;

    /* The UART wants CR LF, as stdio would have sent (hardware-notes.md
     * §2.7's capture reads either, but a terminal does not). */
    unsigned need = (unsigned)n;
    for (int i = 0; i < n; i++) need += line[i] == '\n';

    uint32_t head = s_head;
    if (LOG_RING - (head - s_tail) < need) {
        s_dropped++;
        return;
    }
    for (int i = 0; i < n; i++) {
        if (line[i] == '\n') s_ring[head++ & (LOG_RING - 1u)] = '\r';
        s_ring[head++ & (LOG_RING - 1u)] = line[i];
    }
    __dmb();                    /* the bytes land before the index moves */
    s_head = head;
}

void log_pump(void) {
    uint32_t tail = s_tail;
    uint32_t head = s_head;
    if (tail == head) return;
    __dmb();                    /* read the bytes after seeing the index */
    while (tail != head && uart_is_writable(uart_default)) {
        uart_putc_raw(uart_default, s_ring[tail++ & (LOG_RING - 1u)]);
    }
    __dmb();
    s_tail = tail;
}

unsigned log_dropped(void) {
    return s_dropped;
}
