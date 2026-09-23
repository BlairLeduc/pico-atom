/* inflate.h — gzip decompression into a flat buffer (design.md §11.3).
 *
 * UEF tape images are nearly always gzip files. The whole image is
 * decompressed once, when the tape is inserted, into a buffer the
 * caller owns; the cassette then plays from that buffer. Because the
 * whole output is at hand, back-references are read from it directly
 * and no separate 32 KiB window is kept.
 *
 * Input arrives a byte at a time through a callback, so the port can
 * stream a file off the card through a sector-sized buffer.
 *
 * Not reentrant: the decoder's tables are static, ~1.5 KiB, rather than
 * on the stack of whichever core calls it.
 */
#ifndef PICO_ATOM_INFLATE_H
#define PICO_ATOM_INFLATE_H

#include <stddef.h>
#include <stdint.h>

/* The next input byte, or -1 at the end of the input or on an error. */
typedef int (*inflate_get_fn)(void *ctx);

typedef enum {
    GZ_OK = 0,
    GZ_NOT_GZIP,     /* no gzip magic, or not deflate                 */
    GZ_TRUNCATED,    /* the input ended inside the stream             */
    GZ_BAD_DATA,     /* an invalid deflate stream                     */
    GZ_TOO_BIG,      /* the output does not fit the buffer            */
    GZ_BAD_CHECK,    /* CRC-32 or length in the trailer disagrees     */
} gz_status_t;

/* Decompress one gzip member into out[0..cap). *out_len is the length
 * written, which on an error is how far it got. */
gz_status_t gunzip(inflate_get_fn get, void *ctx, uint8_t *out, size_t cap, size_t *out_len);

const char *gunzip_status_str(gz_status_t st);

#endif /* PICO_ATOM_INFLATE_H */
