/* via6522.h — the 6522 VIA at #B800 (design.md §2.2, §7.3).
 *
 * Fitted by default, and not only for the software that uses its
 * timers: the MOS keeps its printer-enabled flag in the PCR and reads it
 * on every character written (§7.3). A machine without the chip reads
 * open bus there instead, which is #B8 in this model — nonzero in the
 * PCR's CA2 bits — so the MOS believes a printer is enabled and spins
 * for ever on the busy flag at #B801 before the first prompt.
 *
 * What is modelled is what the Atom's software reaches: the port
 * latches and direction registers, both timers, and IFR/IER driving
 * the IRQ line. T1 reloads after latch + 2 cycles in free-run mode,
 * the part's period. The shift register and the four handshake
 * lines are latched and otherwise inert; the complete part is M9's
 * (§17).
 */
#ifndef PICO_ATOM_VIA6522_H
#define PICO_ATOM_VIA6522_H

#include <stdbool.h>
#include <stdint.h>

/* Register indices, a & 15 (§7.3). */
enum {
    VIA_ORB = 0x0, VIA_ORA = 0x1, VIA_DDRB = 0x2, VIA_DDRA = 0x3,
    VIA_T1CL = 0x4, VIA_T1CH = 0x5, VIA_T1LL = 0x6, VIA_T1LH = 0x7,
    VIA_T2CL = 0x8, VIA_T2CH = 0x9, VIA_SR = 0xA, VIA_ACR = 0xB,
    VIA_PCR = 0xC, VIA_IFR = 0xD, VIA_IER = 0xE, VIA_ORA_NH = 0xF,
};

/* IFR / IER bits. */
#define VIA_INT_T1   0x40u
#define VIA_INT_T2   0x20u
#define VIA_INT_ANY  0x80u   /* IFR: set while any enabled flag is set */

#define VIA_ACR_T1_FREERUN  0x40u
#define VIA_ACR_T2_PULSES   0x20u

typedef struct {
    uint8_t orb, ora, ddrb, ddra;
    uint8_t in_a, in_b;        /* pin levels, driven by the machine */
    uint8_t sr, acr, pcr;
    uint8_t ifr, ier;          /* IFR bit 7 is derived, never stored */

    uint16_t t1_latch;
    uint8_t  t2_latch_lo;
    int32_t  t1;               /* counters; below zero means underflowed */
    int32_t  t2;
    bool     t1_armed;         /* one-shot: fire once per T1C-H write */
    bool     t2_armed;
} via6522_t;

void    via6522_reset(via6522_t *v);
uint8_t via6522_read(via6522_t *v, uint8_t reg);   /* reads have side effects */
void    via6522_write(via6522_t *v, uint8_t reg, uint8_t val);

/* Advance both timers. Called once per instruction with its cycle
 * count, so an interrupt is seen at the next instruction boundary,
 * which is when the 6502 samples IRQ anyway. */
void    via6522_tick(via6522_t *v, uint32_t cycles);

static inline bool via6522_irq(const via6522_t *v) {
    return (v->ifr & v->ier & 0x7Fu) != 0;
}

#endif /* PICO_ATOM_VIA6522_H */
