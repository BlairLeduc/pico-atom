/* storage.h — the FAT volume on the SD card (design.md §11.1).
 *
 * Core 1 only, and only at a defined application boundary with the
 * guest paused: card write latency can exceed both the field and the
 * audio deadline (§11.1). Each piece of card work mounts, does its job
 * and unmounts, so a card swapped between two of them is simply a new
 * card.
 */
#ifndef PICO_ATOM_STORAGE_H
#define PICO_ATOM_STORAGE_H

#include <stdbool.h>

/* FatFs's FRESULT, 0 on success. */
int  storage_mount(void);
void storage_unmount(void);

#endif /* PICO_ATOM_STORAGE_H */
