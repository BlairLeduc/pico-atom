/* i8255.c — INS8255 PPI (design.md §2.3). */

#include "i8255.h"

#include "hot.h"

void i8255_reset(i8255_t *p) {
    /* A reset puts all three ports in input mode and clears the latches,
     * which is the part's documented power-on state. */
    p->control = I8255_CTRL_MODE_SET | I8255_CTRL_A_INPUT | I8255_CTRL_B_INPUT |
                 I8255_CTRL_C_HI_INPUT | I8255_CTRL_C_LO_INPUT;
    p->out_a = p->out_b = p->out_c = 0;
    p->in_a  = p->in_b  = p->in_c  = 0xFFu;
}

uint8_t ATOM_HOT1(i8255_read)(const i8255_t *p, uint8_t reg) {
    switch (reg & 3u) {
    case 0:
        return (p->control & I8255_CTRL_A_INPUT) ? p->in_a : p->out_a;
    case 1:
        return (p->control & I8255_CTRL_B_INPUT) ? p->in_b : p->out_b;
    case 2: {
        /* Port C is two independent halves. A read returns the input
         * nibble alongside the last value latched into the output
         * nibble — keeping these apart is what stops a read-modify-write
         * on #B002 corrupting the mode bits (§2.3). */
        uint8_t hi = (p->control & I8255_CTRL_C_HI_INPUT) ? p->in_c : p->out_c;
        uint8_t lo = (p->control & I8255_CTRL_C_LO_INPUT) ? p->in_c : p->out_c;
        return (uint8_t)((hi & 0xF0u) | (lo & 0x0Fu));
    }
    default:
        /* Write-only on the real part; the bus substitutes the open bus. */
        return 0xFFu;
    }
}

void ATOM_HOT1(i8255_write)(i8255_t *p, uint8_t reg, uint8_t v) {
    switch (reg & 3u) {
    case 0: p->out_a = v; break;
    case 1: p->out_b = v; break;
    case 2: p->out_c = v; break;
    default:
        if (v & I8255_CTRL_MODE_SET) {
            /* A mode-set write clears the output latches. */
            p->control = v;
            p->out_a = p->out_b = p->out_c = 0;
        } else {
            /* Bit set/reset: bits 3-1 select the port C bit, bit 0 is
             * the value. This is the path the MOS uses for the speaker
             * and the cassette bits, so it is not optional. */
            uint8_t bit = (uint8_t)(1u << ((v >> 1) & 7u));
            if (v & 1u) p->out_c |= bit;
            else        p->out_c = (uint8_t)(p->out_c & ~bit);
        }
        break;
    }
}
