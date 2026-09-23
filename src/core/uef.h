/* uef.h — a UEF tape image as a sequence of half-cycles (design.md §11.3).
 *
 * UEF describes a tape as chunks: carrier tone, data bytes, gaps. This
 * walks an uncompressed image already in memory and hands out the
 * waveform one half-cycle at a time, in units of a quarter of the base
 * period — 1/4800 s at the standard 1200 Hz — so a 2400 Hz half-cycle
 * is one unit and a 1200 Hz half-cycle two. atom.c turns units into
 * guest cycles; nothing here knows about the guest clock.
 *
 * The Atom records at 300 baud (#FC7C): a 0 is four cycles of 1200 Hz,
 * a 1 is eight cycles of 2400 Hz, and a byte is a 0 start bit, eight
 * data bits LSB first and a 1 stop bit. UEF's own default is 1200 baud,
 * a BBC Micro's; chunk &0117 says which, and an image that does not say
 * is taken to be the Atom's 300, since this is an Atom.
 *
 * Chunks read: &0100 (8N1 data), &0102 (explicit bits), &0104 (defined
 * format), &0110 (carrier), &0111 (carrier with dummy byte), &0112 and
 * &0116 (gaps), &0113 (base frequency), &0114 (security cycles), &0117
 * (baud). Everything else — metadata, &0115's phase — is skipped.
 */
#ifndef PICO_ATOM_UEF_H
#define PICO_ATOM_UEF_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define UEF_HEADER_LEN   12u    /* "UEF File!\0", minor, major        */
#define UEF_BASE_HZ    1200u
#define UEF_BAUD_ATOM   300u

typedef struct {
    const uint8_t *img;
    uint32_t len;

    uint32_t next;         /* offset of the next chunk header            */
    uint16_t id;           /* the chunk being played                     */
    uint32_t body, body_len;
    uint32_t at;           /* bytes (data) or cycles (tone) consumed     */
    uint8_t  part;         /* &0111: tone, dummy byte, tone              */

    /* The frame being sent: its bits, LSB first, and how many are left. */
    uint16_t frame;
    uint8_t  frame_left;
    uint8_t  frame_tail;   /* half-cycles of 2400 Hz after it (&0104)    */

    /* The wave being sent. */
    uint8_t  wave;         /* UEF_WAVE_*                                 */
    uint32_t wave_left;    /* half-cycles, or units for a gap            */

    uint32_t base_hz;      /* &0113                                      */
    uint16_t baud;         /* &0117                                      */
    /* &0104's frame: data bits, parity 'N', 'E' or 'O', stop bits, and
     * whether a negative stop count asked for an extra short wave. */
    uint8_t  fmt_bits, fmt_parity, fmt_stop;
    bool     fmt_short;
} uef_t;

#define UEF_WAVE_NONE 0u
#define UEF_WAVE_HIGH 1u   /* 2400 Hz: half-cycles of one unit  */
#define UEF_WAVE_LOW  2u   /* 1200 Hz: half-cycles of two units */
#define UEF_WAVE_GAP  3u   /* no signal                         */

/* Check the header and rewind. False if this is not a UEF image. */
bool uef_open(uef_t *u, const uint8_t *img, size_t len);
void uef_rewind(uef_t *u);

/* The next piece of the waveform: `units` long, ending in a level
 * change if *edge is true (a half-cycle) and not if it is false (a
 * gap). False at the end of the tape. */
bool uef_next(uef_t *u, uint32_t *units, bool *edge);

/* The name in the first block header on the tape — the "****", up to
 * 13 characters, CR that the MOS writes (#FB3B) — so the menu can say
 * what to LOAD. An empty name means the ROM's nameless format; false if
 * the image has no such header. */
bool uef_first_name(const uint8_t *img, size_t len, char name[14]);

/* How far through the image, 0-100, for the menu. */
unsigned uef_percent(const uef_t *u);

#endif /* PICO_ATOM_UEF_H */
