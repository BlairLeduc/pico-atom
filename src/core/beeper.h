/* beeper.h — the loudspeaker bit to PCM (design.md §9.1, §9.3).
 *
 * The Atom's only sound is port C bit 2, toggled by the CPU. A sample is
 * the time average of that bit over the guest cycles the sample covers —
 * a box filter at exactly the sample period — not a point sample of it,
 * which aliases audibly (§9.3). The cost is per toggle plus a short loop
 * per sample, and it is exact for square waves of any frequency.
 *
 * Time is kept as a rational, never a truncated rate (§9.2,
 * hardware-notes.md §5.2): guest cycles per sample is num/den, and the
 * sample boundaries are tracked in units of 1/den of a guest cycle, so
 * the boundaries land where they should for ever rather than drifting.
 *
 * Output is signed 16-bit mono through a one-pole DC blocker, so a
 * speaker left high and a speaker left low both come to rest at 0, which
 * is the silence value the port pads an underrun with (§12.2).
 */
#ifndef PICO_ATOM_BEEPER_H
#define PICO_ATOM_BEEPER_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

/* Speaker fully high for a whole sample, before the DC blocker. A step
 * through the blocker swings at most this far either side of 0, which
 * leaves int16 a factor of two of headroom. */
#define BEEPER_FULL_SCALE 16384

typedef struct {
    /* Guest cycles per sample = num / den = q + r / den. */
    uint32_t num, den, q, r;
    uint64_t scale;          /* (FULL_SCALE << 32) / num, rounded       */

    /* The current sample began at cycle `start` plus start_frac / den. */
    uint64_t start;
    uint32_t start_frac;
    uint32_t pos;            /* units (1/den cycle) of it accounted for */
    uint32_t high;           /* ...of which the speaker was high        */
    bool     level;

    bool     dc_block;       /* on by default; tests turn it off        */
    int32_t  hp_x;           /* last input                              */
    int32_t  hp_y;           /* last output, Q8                         */

    int16_t  buf[ATOM_AUDIO_BUF_LEN];
    uint32_t count;
    uint32_t overflow;       /* samples lost because nobody drained     */
    uint32_t edges;          /* speaker transitions, for the soak (§15.3) */
} beeper_t;

/* Start at guest cycle `now` with the speaker at `level`. The sample
 * rate is rate_num / rate_den Hz — on the device clk_sys over
 * (TOP + 1) x oversample x divider, passed as that fraction so nothing
 * is rounded (§9.2). */
void beeper_init(beeper_t *b, uint64_t now, bool level, uint32_t cpu_hz,
                 uint32_t rate_num, uint32_t rate_den);

/* The speaker bit was written at guest cycle `now`. */
void beeper_set_level(beeper_t *b, uint64_t now, bool level);

/* Emit every sample that ends at or before guest cycle `now`. */
void beeper_advance(beeper_t *b, uint64_t now);

/* Move up to `max` samples out, oldest first. Returns how many. */
size_t beeper_drain(beeper_t *b, int16_t *dst, size_t max);

#endif /* PICO_ATOM_BEEPER_H */
