/* m6502.h — MOS 6502 interpreter (design.md §6).
 *
 * Cycle-correct at instruction granularity: every documented opcode
 * consumes the right number of cycles including page-crossing and branch
 * penalties, and decimal mode is exact.
 *
 * The CPU has no notion of a device. It reaches memory through bus.h,
 * and the bus layer is what advances devices (§6.2).
 */
#ifndef PICO_ATOM_M6502_H
#define PICO_ATOM_M6502_H

#include <stdbool.h>
#include <stdint.h>

struct atom_s;

/* Processor status bits. */
#define M6502_C 0x01u
#define M6502_Z 0x02u
#define M6502_I 0x04u
#define M6502_D 0x08u
#define M6502_B 0x10u
#define M6502_U 0x20u   /* unused, reads as 1 */
#define M6502_V 0x40u
#define M6502_N 0x80u

/* Vectors. */
#define M6502_VEC_NMI 0xFFFAu
#define M6502_VEC_RES 0xFFFCu
#define M6502_VEC_IRQ 0xFFFEu

/* IRQ sources, OR-ed into irq_lines so they compose correctly (§6.4). */
#define M6502_IRQ_VIA 0x01u
#define M6502_IRQ_FDC 0x02u

typedef struct {
    uint16_t pc;
    uint8_t  a, x, y, s, p;

    uint64_t cycles;

    uint8_t  irq_lines;      /* bitmask of asserted IRQ sources; 0 deasserts */
    bool     nmi_pending;    /* edge-triggered, latched                     */
    bool     nmi_line;       /* last level seen, for edge detection         */
    bool     reset_pending;  /* BREAK key (§6.4)                            */

    /* Undocumented opcodes: v1 traps and logs (§6.1). */
    uint32_t undoc_count;
    uint16_t undoc_pc;       /* PC of the most recent trapped opcode        */
    uint8_t  undoc_op;
} m6502_t;

void     m6502_init(m6502_t *c);
void     m6502_reset(m6502_t *c, struct atom_s *m);

/* Execute exactly one instruction (servicing a pending reset or interrupt
 * first, at the instruction boundary). Returns the cycles consumed. */
uint32_t m6502_step(struct atom_s *m);

/* Interrupt line control. NMI is edge-triggered: a false->true transition
 * latches. IRQ is level-sensitive. */
void     m6502_set_nmi(m6502_t *c, bool level);
void     m6502_set_irq(m6502_t *c, uint8_t source, bool asserted);

/* Base cycle count for an opcode, before page-cross and branch penalties.
 * Exposed so the host cycle-table test can assert against it (§15.1);
 * undocumented opcodes report 0. */
uint8_t  m6502_base_cycles(uint8_t opcode);
bool     m6502_is_documented(uint8_t opcode);

#endif /* PICO_ATOM_M6502_H */
