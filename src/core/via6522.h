/* via6522.h — the 6522 VIA at #B800 (design.md §2.2, §7.3).
 *
 * Fitted by default, and not only for the software that uses its
 * timers: the MOS keeps its printer-enabled flag in the PCR and reads it
 * on every character written (§7.3). A machine without the chip reads
 * open bus there instead, which is #B8 in this model — nonzero in the
 * PCR's CA2 bits — so the MOS believes a printer is enabled and spins
 * for ever on the busy flag at #B801 before the first prompt.
 *
 * The whole part, from M10 (§7.4): both ports with input latching, the
 * four control lines in every PCR mode (edge interrupts, handshake and
 * pulse outputs, manual levels), T1 one-shot and free-run with its PB7
 * output, T2 one-shot and counting pulses on PB6, and the shift
 * register in all eight modes. T1 reloads after latch + 2 cycles in
 * free-run mode, the part's period.
 *
 * Nothing on an Atom drives the control lines or the user port, so the
 * outside world is a set of calls: via6522_set_ca1() and its kind put a
 * level on a pin, and the outputs are fields to read (`pa_out()`,
 * `pb_out()`, `ca2`, `cb1`, `cb2`). Timing is to the instruction: the
 * machine ticks the chip once per instruction with its cycle count.
 *
 * The tick costs M9's or less whatever the chip is doing (§6.3): it
 * counts T1 and one countdown to the next thing T2 or the shift
 * register will do, and does the rest out of line when either runs
 * out. So `t2` and `sr_timer` lag between those events; the register
 * reads that show them bring them up to date, and anything else that
 * looks at them calls via6522_sync() first.
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
#define VIA_INT_CA2  0x01u
#define VIA_INT_CA1  0x02u
#define VIA_INT_SR   0x04u
#define VIA_INT_CB2  0x08u
#define VIA_INT_CB1  0x10u
#define VIA_INT_T2   0x20u
#define VIA_INT_T1   0x40u
#define VIA_INT_ANY  0x80u   /* IFR: set while any enabled flag is set */

/* ACR. */
#define VIA_ACR_PA_LATCH    0x01u
#define VIA_ACR_PB_LATCH    0x02u
#define VIA_ACR_SR_MASK     0x1Cu
#define VIA_ACR_SR_SHIFT    2u
#define VIA_ACR_T2_PULSES   0x20u
#define VIA_ACR_T1_FREERUN  0x40u
#define VIA_ACR_T1_PB7      0x80u

/* The shift register's eight modes, ACR bits 4:2. */
enum {
    VIA_SR_OFF, VIA_SR_IN_T2, VIA_SR_IN_PHI2, VIA_SR_IN_EXT,
    VIA_SR_OUT_FREE, VIA_SR_OUT_T2, VIA_SR_OUT_PHI2, VIA_SR_OUT_EXT,
};

typedef struct {
    uint8_t orb, ora, ddrb, ddra;
    uint8_t in_a, in_b;        /* pin levels, driven by the machine */
    uint8_t ira, irb;          /* input latches, taken on CA1 / CB1 */
    uint8_t sr, acr, pcr;
    uint8_t ifr, ier;          /* IFR bit 7 is derived, never stored */

    uint16_t t1_latch;
    uint8_t  t2_latch_lo;
    int32_t  t1;               /* counters; below zero means underflowed */
    int32_t  t2;
    bool     t1_armed;         /* one-shot: fire once per T1C-H write */
    bool     t2_armed;
    bool     pb7;              /* T1's output on PB7 when ACR bit 7 is set */

    /* The control lines' levels. CA1 is always an input; CB1 is the
     * shift clock's output in the internally clocked modes. CA2 and CB2
     * are the pin's level whichever way the PCR points them. */
    bool     ca1, ca2, cb1, cb2;

    /* The shift register's progress: CB1 half-cycles left in this
     * byte (16 at the start, 0 when stopped), and cycles to the next
     * one in the internally clocked modes. */
    uint8_t  sr_halves;
    int16_t  sr_timer;

    /* Cycles, less one, to T2's underflow or the shift clock's next
     * edge, whichever is sooner; the tick counts it down. `ev_span` is
     * what it was set to, so the cycles since are ev_span - ev. */
    int32_t  ev, ev_span;

    /* Pulses put out on CA2 and CB2 in pulse mode; a pulse lasts one
     * cycle, which is shorter than anything here can see as a level. */
    uint16_t ca2_pulses, cb2_pulses;
} via6522_t;

void    via6522_reset(via6522_t *v);
uint8_t via6522_read(via6522_t *v, uint8_t reg);   /* reads have side effects */
void    via6522_write(via6522_t *v, uint8_t reg, uint8_t val);

/* Advance the timers and the shift register. Called once per
 * instruction with its cycle count, so an interrupt is seen at the next
 * instruction boundary, which is when the 6502 samples IRQ anyway. */
void    via6522_tick(via6522_t *v, uint32_t cycles);

/* Bring `t2` and `sr_timer` up to date, for a reader of the struct. */
void    via6522_sync(via6522_t *v);

/* After storing the fields directly (a snapshot load): count from them. */
void    via6522_rearm(via6522_t *v);

/* The outside world. A level that does not change does nothing; an
 * active edge sets its flag, latches its port, ends a handshake, or
 * clocks the shift register, as the PCR and ACR say. */
void    via6522_set_ca1(via6522_t *v, bool level);
void    via6522_set_ca2(via6522_t *v, bool level);   /* ignored while CA2 is an output */
void    via6522_set_cb1(via6522_t *v, bool level);   /* ignored while CB1 clocks out */
void    via6522_set_cb2(via6522_t *v, bool level);   /* ignored while CB2 is an output */
void    via6522_set_pa(via6522_t *v, uint8_t pins);
void    via6522_set_pb(via6522_t *v, uint8_t pins);  /* PB6 falling edges count T2 */

static inline uint8_t via6522_sr_mode(const via6522_t *v) {
    return (uint8_t)((v->acr & VIA_ACR_SR_MASK) >> VIA_ACR_SR_SHIFT);
}

/* What the chip drives onto its ports: output bits as latched, inputs
 * pulled up, and PB7 from T1 when ACR bit 7 gives it the pin. */
static inline uint8_t via6522_pa_out(const via6522_t *v) {
    return (uint8_t)((v->ora & v->ddra) | (uint8_t)~v->ddra);
}
static inline uint8_t via6522_pb_out(const via6522_t *v) {
    uint8_t pb = (uint8_t)((v->orb & v->ddrb) | (uint8_t)~v->ddrb);
    if (v->acr & VIA_ACR_T1_PB7) pb = (uint8_t)((pb & 0x7Fu) | (v->pb7 ? 0x80u : 0u));
    return pb;
}

static inline bool via6522_irq(const via6522_t *v) {
    return (v->ifr & v->ier & 0x7Fu) != 0;
}

#endif /* PICO_ATOM_VIA6522_H */
