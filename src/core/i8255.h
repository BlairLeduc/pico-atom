/* i8255.h — INS8255 PPI at #B000 (design.md §2.3).
 *
 * This single chip is the whole of the Atom's I/O: keyboard, VDG mode,
 * loudspeaker and cassette. The model is the generic Intel part — the
 * direction bits are decoded from the control word rather than assuming
 * the Atom's usual programming — with the Atom's wiring expressed as
 * accessors at the bottom of this header.
 *
 * Two things this model exists to get right (§2.3):
 *   - port C is bidirectional per nibble, so the output latches are kept
 *     separate from the input sources; otherwise a read-modify-write in
 *     the MOS corrupts the mode bits;
 *   - bit set/reset through the control register is how the MOS toggles
 *     the speaker and the cassette bits, so the BSR path is implemented,
 *     not just the mode path.
 */
#ifndef PICO_ATOM_I8255_H
#define PICO_ATOM_I8255_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    /* Output latches. A bit is visible on a read only when its half of
     * the chip is configured as an output. */
    uint8_t out_a, out_b, out_c;

    /* Input sources, driven by the rest of the machine. */
    uint8_t in_a, in_b, in_c;

    uint8_t control;   /* last mode-set control word */
} i8255_t;

/* Control word (bit 7 set). Mode 0 is the only mode the Atom uses, but
 * the direction bits are honoured whatever mode is selected. */
#define I8255_CTRL_MODE_SET   0x80u
#define I8255_CTRL_A_INPUT    0x10u
#define I8255_CTRL_C_HI_INPUT 0x08u
#define I8255_CTRL_B_INPUT    0x02u
#define I8255_CTRL_C_LO_INPUT 0x01u

void    i8255_reset(i8255_t *p);
uint8_t i8255_read(const i8255_t *p, uint8_t reg);
void    i8255_write(i8255_t *p, uint8_t reg, uint8_t v);

/* The control register is write-only on the real part, so a read of
 * #B003 is not driven by the chip; the bus returns the open bus for it. */
static inline bool i8255_reg_drives_bus(uint8_t reg) { return (reg & 3u) != 3u; }

/* ---- the Atom's wiring (design.md §2.3) ----------------------------- */

/* Port A bits 0-3: keyboard column select, 0-9. */
static inline uint8_t i8255_kbd_column(const i8255_t *p) { return p->out_a & 0x0Fu; }

/* Port A bits 4-7: the VDG mode nibble. Which bit is which is §16's
 * medium-confidence item, so the decode lives in mc6847.c and this
 * accessor hands over the raw nibble rather than interpreting it. */
static inline uint8_t i8255_vdg_nibble(const i8255_t *p) { return p->out_a & 0xF0u; }

/* Port C outputs. */
static inline bool i8255_cassette_out(const i8255_t *p) { return (p->out_c & 0x01u) != 0; }
static inline bool i8255_cassette_tone(const i8255_t *p) { return (p->out_c & 0x02u) != 0; }
static inline bool i8255_speaker(const i8255_t *p)       { return (p->out_c & 0x04u) != 0; }
static inline bool i8255_css(const i8255_t *p)           { return (p->out_c & 0x08u) != 0; }

/* Port C inputs. REPT is active low; FS is the 60 Hz flyback flag. */
#define I8255_IN_C_CASSETTE_TONE 0x10u
#define I8255_IN_C_CASSETTE_DATA 0x20u
#define I8255_IN_C_REPT          0x40u
#define I8255_IN_C_FS            0x80u

#endif /* PICO_ATOM_I8255_H */
