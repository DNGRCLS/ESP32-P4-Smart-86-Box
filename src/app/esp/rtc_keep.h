#pragma once
#include <stdbool.h>
#include <stddef.h>

/* Battery-backed clock.
 *
 * The panel's "RTC battery" header feeds the ESP32-P4 VBAT pin, which can keep the chip's
 * low-power domain (its RTC counter and LP memory) running while the main supply is off.
 * rtc_keep saves "wall time at RTC tick N" into LP memory every 30 s and, at power-up,
 * works out the current time from how far the RTC counter has advanced.  The counter's
 * rate is calibrated against network time while online.
 *
 * A DS3231 RTC module on the I2C bus (address 0x68) is used instead when one is found.
 */
void rtc_keep_boot(void);              /* early in boot: enables VBAT switch-over, restores the time */
void rtc_keep_start(void);             /* starts the 30 s save task */
void rtc_keep_ntp_synced(void);        /* call from the SNTP sync callback */
void rtc_keep_vbat_changed(void);      /* the setting changed */
void rtc_keep_status(char *out, size_t n);   /* one or two human-readable lines */
bool rtc_keep_restored(void);          /* the running time came from the battery clock / module */
