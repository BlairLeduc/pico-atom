/* snapshot.h — whole-machine state to and from a byte stream
 * (design.md §11.5).
 *
 * The format is explicit, field by field and little-endian, never a
 * struct dumped from memory: atom_t holds pointers and padding, and a
 * snapshot has to outlive the build that wrote it.
 *
 *   header  20 bytes: "PATMSNAP" (8), version (2), header length (2),
 *           payload length (4), CRC-32 of the payload (4)
 *   payload the machine's state (SNAP_STATE_LEN), then the whole 64 KiB
 *           address space with ROM pages written as zeros
 *
 * ROM bytes never go into a snapshot: Acorn's images are the user's to
 * supply (§1), and a snapshot is a file the user may pass around. What
 * goes in instead is the SHA-1 of every ROM page, and a snapshot loads
 * only into a machine whose ROMs hash the same, because a program
 * resumed over different ROMs is §16's worst kind of bug.
 *
 * I/O is through a callback, so the core never sees a file. Loading is
 * two passes: snapshot_check reads the whole stream and verifies the
 * header, the CRC, the configuration and the ROMs without touching the
 * machine; only then does snapshot_load, over the same bytes again,
 * change anything. A torn or foreign file therefore leaves the running
 * machine exactly as it was.
 */
#ifndef PICO_ATOM_SNAPSHOT_H
#define PICO_ATOM_SNAPSHOT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "config.h"

struct atom_s;

#define SNAP_VERSION     1u
#define SNAP_HEADER_LEN  20u
#define SNAP_STATE_LEN   96u
#define SNAP_PAYLOAD_LEN (SNAP_STATE_LEN + ATOM_ADDR_SPACE)
#define SNAP_FILE_LEN    (SNAP_HEADER_LEN + SNAP_PAYLOAD_LEN)

typedef enum {
    SNAP_OK = 0,
    SNAP_IO,              /* the callback failed                        */
    SNAP_NOT_SNAPSHOT,    /* wrong magic or lengths                     */
    SNAP_NEWER,           /* a version this build does not know         */
    SNAP_CORRUPT,         /* the CRC does not match                     */
    SNAP_OTHER_MACHINE,   /* RAM populated differently, or another rate */
    SNAP_OTHER_ROMS,      /* the ROMs fitted are not the ones it ran on */
    SNAP_BUSY,            /* a tape call, or the FDC mid-command        */
} snap_status_t;

/* Move exactly n bytes; false on any failure. */
typedef bool (*snap_write_fn)(void *ctx, const uint8_t *src, size_t n);
typedef bool (*snap_read_fn)(void *ctx, uint8_t *dst, size_t n);

snap_status_t snapshot_save(const struct atom_s *m, snap_write_fn write, void *ctx);
snap_status_t snapshot_check(const struct atom_s *m, snap_read_fn read, void *ctx);
snap_status_t snapshot_load(struct atom_s *m, snap_read_fn read, void *ctx);

const char *snapshot_status_str(snap_status_t st);

/* CRC-32 (IEEE 802.3, reflected, as zlib computes it), exposed for the
 * test that pins it against a known value. */
uint32_t snapshot_crc32(uint32_t crc, const uint8_t *p, size_t n);

#endif /* PICO_ATOM_SNAPSHOT_H */
