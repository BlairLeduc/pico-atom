/* storage.c — the FAT volume on the SD card (storage.h). */

#include "storage.h"

#include "ff.h"

static FATFS s_fs;

int storage_mount(void) {
    return (int)f_mount(&s_fs, "", 1);
}

void storage_unmount(void) {
    f_unmount("");
}
