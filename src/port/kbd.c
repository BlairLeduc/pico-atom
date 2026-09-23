/* kbd.c — southbridge key events to core 0 (design.md §10.2). */

#include "kbd.h"

#include "hardware/sync.h"

#include "config.h"
#include "southbridge.h"

#define SB_FIFO_DEPTH 31u   /* hardware-notes.md §6.1 */

/* Power of two, so the indices can run free and wrap by mask. */
#define RING 64u
_Static_assert(RING >= ATOM_KEY_EVENT_QUEUE && (RING & (RING - 1u)) == 0,
               "the ring must hold a full queue and be a power of two");

static uint16_t          s_ring[RING];
static volatile uint32_t s_head;       /* written by core 1 only */
static volatile uint32_t s_tail;       /* written by core 0 only */
static volatile uint32_t s_overflows;

unsigned kbd_poll(void) {
    unsigned n = 0;
    for (; n < SB_FIFO_DEPTH; n++) {
        uint8_t r[2];
        if (sb_read(SB_REG_FIF, r) != SB_OK) break;
        if (r[0] == 0 && r[1] == 0) break;          /* empty */

        uint32_t head = s_head;
        if (head - s_tail >= RING) {
            s_overflows++;
            continue;
        }
        s_ring[head % RING] = (uint16_t)(r[0] << 8 | r[1]);
        __dmb();                /* the entry lands before the index moves */
        s_head = head + 1u;
    }
    return n;
}

bool kbd_pop(uint8_t *state, uint8_t *code) {
    uint32_t tail = s_tail;
    if (tail == s_head) return false;
    __dmb();                    /* read the entry after seeing the index */
    uint16_t e = s_ring[tail % RING];
    __dmb();                    /* and finish reading before freeing it */
    s_tail = tail + 1u;
    *state = (uint8_t)(e >> 8);
    *code  = (uint8_t)e;
    return true;
}

uint32_t kbd_overflows(void) {
    return s_overflows;
}
