/* snapio.h — snapshot slots on the card: /atom/snaps/slotN.psnap
 * (design.md §11.5).
 *
 * Core 1 only, with the guest parked (main.c), like all card work
 * (§11.1). A save writes slotN.new, closes it, removes slotN.psnap and
 * renames the new file into place: a rename alone is not proof of
 * power-loss atomicity (hardware-notes.md §7.1), so the recovery policy
 * is on the load side. A load checks slotN.psnap whole — header, CRC,
 * machine, ROMs (snapshot.h) — and if it is missing or damaged falls
 * back to a whole slotN.new, which is what an interrupted publish
 * leaves. Nothing changes in the machine until one of them has passed.
 */
#ifndef PICO_ATOM_SNAPIO_H
#define PICO_ATOM_SNAPIO_H

#include <stdbool.h>

#include "atom.h"
#include "snapshot.h"

#define SNAPIO_SLOTS 4u

/* The card must be mounted (storage.h) for all of these. */
snap_status_t snapio_save(const atom_t *m, unsigned slot);
snap_status_t snapio_load(atom_t *m, unsigned slot, bool *recovered);
bool          snapio_exists(unsigned slot);
bool          snapio_delete(unsigned slot);

#endif /* PICO_ATOM_SNAPIO_H */
