/*
 * microSD card (4-bit SDMMC slot 0: CLK 43, CMD 44, D0-D3 39-42) mounted at /sdcard through the
 * Arduino SD_MMC library.  Everything else uses plain C stdio / dirent calls on that path.
 */
#include <Arduino.h>
#include <SD_MMC.h>
#include "sdcard.h"

static bool s_mounted;

extern "C" bool sdcard_mount(char *err, int errlen)
{
    if (s_mounted) {
        SD_MMC.end();
        s_mounted = false;
    }
    SD_MMC.setPins(43, 44, 39, 40, 41, 42);
    /* The VO4 rail (3.3 V for GPIO 39-48) is already switched on by board init, so the library
     * must not try to claim the LDO for itself. */
    SD_MMC.setPowerChannel(-1);
    bool ok = SD_MMC.begin(SDCARD_MOUNT, false, false, SDMMC_FREQ_HIGHSPEED, 6);
    if (!ok) {
        SD_MMC.end();
        delay(50);
        ok = SD_MMC.begin(SDCARD_MOUNT, false, false, SDMMC_FREQ_DEFAULT, 6);
    }
    if (!ok) {
        snprintf(err, errlen, "No card, or it is not FAT32 formatted");
        return false;
    }
    if (SD_MMC.cardType() == CARD_NONE) {
        SD_MMC.end();
        snprintf(err, errlen, "No card inserted");
        return false;
    }
    s_mounted = true;
    return true;
}

extern "C" void sdcard_unmount(void)
{
    if (s_mounted) SD_MMC.end();
    s_mounted = false;
}

extern "C" uint64_t sdcard_total_bytes(void) { return s_mounted ? SD_MMC.totalBytes() : 0; }
extern "C" uint64_t sdcard_used_bytes(void) { return s_mounted ? SD_MMC.usedBytes() : 0; }
