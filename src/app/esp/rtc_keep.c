/* Battery-backed clock - see rtc_keep.h.
 *
 * How the ESP32-P4 keeps time without main power
 * ----------------------------------------------
 * The chip's low-power (LP) domain holds a 48-bit RTC counter (clocked by the internal
 * ~136 kHz RC oscillator on this board) and 32 KB of LP memory.  With the VBAT pin
 * switch-over set to "automatic", the PMU moves the LP domain onto the backup battery
 * when the main 3.3 V supply drops, so the counter keeps running and LP memory keeps
 * its contents.  ESP-IDF itself resets its notion of time on a power-on reset, so we
 * keep our own record:
 *
 *   slot = { unix time in us, RTC counter value, RTC tick period in us }
 *
 * written every 30 s (two slots, alternating, each with a CRC so a write torn by the
 * power going away is ignored).  At power-up:  now = slot.time + (counter - slot.counter)
 * * period.  The tick period is measured against network time while online (the RC
 * oscillator's factory calibration is only good to a few tenths of a percent).
 *
 * Espressif documents VBAT on the P4 as experimental (verified by them in deep sleep),
 * so every outcome is reported in Settings > Time and on the web page.
 */
#include "rtc_keep.h"
#include <stdio.h>
#include <string.h>
#include <sys/time.h>
#include <time.h>
#include "board.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_private/esp_clk.h"
#include "esp_rom_crc.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "soc/pmu_reg.h"
#include "soc/rtc.h"
#include "soc/soc.h"
#include "../core/settings.h"

static const char *TAG = "rtc";

#define VALID_EPOCH   1735689600LL      /* 2025-01-01: anything earlier is "clock not set" */
#define SAVE_PERIOD_S 30
#define KEEP_MAGIC    0x50345254u       /* 'P4RT' */
#define DS3231_ADDR   0x68

typedef struct {
    uint32_t magic;
    uint32_t seq;
    int64_t epoch_us;       /* unix time ... */
    uint64_t ticks;         /* ... at this RTC counter value */
    double period_us;       /* RTC tick length */
    uint32_t learned;       /* period measured against network time */
    uint32_t crc;
} keep_slot_t;

/* LP memory: survives restarts, and power cuts while VBAT holds the LP domain up */
static RTC_NOINIT_ATTR keep_slot_t s_slot[2];

typedef enum {
    BOOT_UNKNOWN = 0,
    BOOT_FIRST,         /* nothing saved yet by this firmware */
    BOOT_KEPT,          /* restart without a power cut: IDF kept the time */
    BOOT_RESTORED,      /* power cut, time rebuilt from the battery-held counter */
    BOOT_LOST_COUNT,    /* LP memory held but the counter restarted */
    BOOT_LOST,          /* nothing held across the power cut */
    BOOT_EXT,           /* time read from the DS3231 module */
} boot_t;

static SemaphoreHandle_t s_mtx;
static TaskHandle_t s_task;
static uint32_t s_seq;
static double s_period;             /* current best tick period, us */
static bool s_learned;
static boot_t s_boot;
static int64_t s_off_s;             /* how long the power was off (restored boots) */
static bool s_err_pending;          /* measure the restore error at the first network sync */
static bool s_err_valid;
static double s_err_s;              /* network time minus battery-clock time, seconds */
static bool s_restored;
static bool s_vbat_on;
static uint32_t s_ana_mode;         /* PMU_ANA_VDDBAT_MODE read back */
static bool s_saved_once;
static time_t s_last_save;
static bool s_learn0;               /* first network sync of this session */
static int64_t s_l0_us;
static uint64_t s_l0_ticks;
static int64_t s_last_ntp_us;
static uint64_t s_last_ntp_ticks;

/* DS3231 */
static i2c_master_dev_handle_t s_ds;
static bool s_ds_present, s_ds_valid;
static volatile bool s_ds_write_req;
static time_t s_ds_last_write;

/* ------------------------------------------------------------------ helpers */

static uint32_t slot_crc(const keep_slot_t *s)
{
    return esp_rom_crc32_le(0, (const uint8_t *)s, offsetof(keep_slot_t, crc));
}

static bool slot_ok(const keep_slot_t *s)
{
    return s->magic == KEEP_MAGIC && s->crc == slot_crc(s) && s->period_us > 1.0 && s->period_us < 100.0;
}

static const keep_slot_t *slot_latest(void)
{
    bool a = slot_ok(&s_slot[0]), b = slot_ok(&s_slot[1]);
    if (a && b) return (int32_t)(s_slot[1].seq - s_slot[0].seq) > 0 ? &s_slot[1] : &s_slot[0];
    return a ? &s_slot[0] : b ? &s_slot[1] : NULL;
}

static double factory_period(void)
{
    uint32_t cal = esp_clk_slowclk_cal_get();         /* us per tick, Q19 */
    return cal ? (double)cal / (double)(1 << RTC_CLK_CAL_FRACT) : 1e6 / 136000.0;
}

static int64_t now_us(void)
{
    struct timeval tv;
    gettimeofday(&tv, NULL);
    return (int64_t)tv.tv_sec * 1000000LL + tv.tv_usec;
}

static bool time_valid(void) { return time(NULL) > VALID_EPOCH; }

static void save_locked(void)
{
    if (!time_valid()) return;
    keep_slot_t s = {0};
    s.magic = KEEP_MAGIC;
    s.seq = ++s_seq;
    uint64_t t0 = rtc_time_get();
    s.epoch_us = now_us();
    uint64_t t1 = rtc_time_get();
    s.ticks = t0 + (t1 - t0) / 2;
    s.period_us = s_period;
    s.learned = s_learned;
    s.crc = slot_crc(&s);
    s_slot[s.seq & 1] = s;
    s_last_save = time(NULL);
    if (!s_saved_once) {
        s_saved_once = true;
        nvs_handle_t h;                     /* remember that a record exists, to tell "lost" from "first" */
        if (nvs_open("p4box", NVS_READWRITE, &h) == ESP_OK) {
            uint8_t v = 0;
            if (nvs_get_u8(h, "rtc_armed", &v) != ESP_OK || v != 1) {
                nvs_set_u8(h, "rtc_armed", 1);
                nvs_commit(h);
            }
            nvs_close(h);
        }
    }
}

static bool nvs_armed(void)
{
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open("p4box", NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "rtc_armed", &v);
        nvs_close(h);
    }
    return v == 1;
}

static void fmt_dur(char *out, size_t n, int64_t s)
{
    if (s < 0) s = -s;
    if (s < 90) snprintf(out, n, "%lld s", (long long)s);
    else if (s < 5400) snprintf(out, n, "%lld min", (long long)((s + 30) / 60));
    else if (s < 48 * 3600) snprintf(out, n, "%lld h %lld min", (long long)(s / 3600), (long long)((s % 3600) / 60));
    else snprintf(out, n, "%lld days %lld h", (long long)(s / 86400), (long long)((s % 86400) / 3600));
}

/* ------------------------------------------------------------------ VBAT switch-over */

static void vbat_apply(bool on)
{
    /* LP-active power config: VDDBAT mode 0 = always main supply, 1 = always VBAT,
     * 2 = automatic (main supply while present, VBAT when it drops). */
    REG_SET_FIELD(PMU_HP_SLEEP_LP_DIG_POWER_REG, PMU_HP_SLEEP_VDDBAT_MODE, on ? 2 : 0);
    REG_SET_BIT(PMU_VDDBAT_CFG_REG, PMU_VDDBAT_SW_UPDATE);
    for (volatile int i = 0; i < 1000; i++) {}
    s_ana_mode = REG_GET_FIELD(PMU_VDDBAT_CFG_REG, PMU_ANA_VDDBAT_MODE);
    s_vbat_on = on;
    ESP_LOGI(TAG, "VBAT switch-over %s (mode readback %u)", on ? "automatic" : "off", (unsigned)s_ana_mode);
}

/* ------------------------------------------------------------------ DS3231 module */

static int bcd2(uint8_t v) { return (v >> 4) * 10 + (v & 0x0F); }
static uint8_t tobcd(int v) { return (uint8_t)(((v / 10) << 4) | (v % 10)); }

static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

static bool ds_read(uint8_t reg, uint8_t *buf, size_t n)
{
    return i2c_master_transmit_receive(s_ds, &reg, 1, buf, n, 100) == ESP_OK;
}

static void ds_probe(void)
{
    if (board_i2c_init() != ESP_OK || !board_i2c()) return;
    if (i2c_master_probe(board_i2c(), DS3231_ADDR, 50) != ESP_OK) return;
    i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = DS3231_ADDR, .scl_speed_hz = 100000};
    if (i2c_master_bus_add_device(board_i2c(), &dc, &s_ds) != ESP_OK) return;
    uint8_t r[19];
    if (!ds_read(0, r, sizeof(r))) goto not_ds;
    /* DS3231 fingerprint: valid BCD time, temperature LSB register only uses bits 7..6 */
    if ((r[0] & 0x7F) > 0x59 || (r[1] & 0x7F) > 0x59 || (r[0] & 0x0F) > 9 || (r[0x12] & 0x3F) != 0) goto not_ds;
    int8_t temp = (int8_t)r[0x11];
    if (temp < -40 || temp > 85) goto not_ds;
    s_ds_present = true;
    bool osf = r[0x0F] & 0x80;              /* oscillator stopped: battery ran out / never set */
    int sec = bcd2(r[0] & 0x7F), min = bcd2(r[1] & 0x7F), hour;
    if (r[2] & 0x40) {                      /* 12-hour mode */
        hour = bcd2(r[2] & 0x1F) % 12 + ((r[2] & 0x20) ? 12 : 0);
    } else {
        hour = bcd2(r[2] & 0x3F);
    }
    int day = bcd2(r[4] & 0x3F), mon = bcd2(r[5] & 0x1F), year = 2000 + bcd2(r[6]) + ((r[5] & 0x80) ? 100 : 0);
    if (osf || mon < 1 || mon > 12 || day < 1 || day > 31 || hour > 23) {
        ESP_LOGW(TAG, "DS3231 found, time not valid (OSF %d)", osf);
        return;
    }
    int64_t t = days_from_civil(year, mon, day) * 86400 + hour * 3600 + min * 60 + sec;
    if (t < VALID_EPOCH) return;
    s_ds_valid = true;
    struct timeval tv = {.tv_sec = (time_t)t, .tv_usec = 500000};
    settimeofday(&tv, NULL);
    ESP_LOGI(TAG, "DS3231 found (%d C), time %04d-%02d-%02d %02d:%02d:%02d UTC", temp, year, mon, day, hour, min, sec);
    return;
not_ds:
    i2c_master_bus_rm_device(s_ds);
    s_ds = NULL;
}

static void ds_write(void)
{
    if (!s_ds_present || !time_valid()) return;
    /* write on a second boundary so the module ticks in step */
    struct timeval tv;
    gettimeofday(&tv, NULL);
    vTaskDelay(pdMS_TO_TICKS((1000000 - tv.tv_usec) / 1000 + 1));
    time_t now = time(NULL);
    struct tm g;
    gmtime_r(&now, &g);
    uint8_t b[8] = {0x00, tobcd(g.tm_sec), tobcd(g.tm_min), tobcd(g.tm_hour), (uint8_t)(g.tm_wday + 1),
                    tobcd(g.tm_mday), (uint8_t)(tobcd(g.tm_mon + 1) | (g.tm_year >= 200 ? 0x80 : 0)), tobcd(g.tm_year % 100)};
    if (i2c_master_transmit(s_ds, b, sizeof(b), 100) != ESP_OK) return;
    uint8_t st;
    if (ds_read(0x0F, &st, 1)) {
        uint8_t w[2] = {0x0F, (uint8_t)(st & ~0x80)};     /* clear OSF */
        i2c_master_transmit(s_ds, w, 2, 100);
    }
    s_ds_valid = true;
    s_ds_last_write = now;
    ESP_LOGI(TAG, "DS3231 set from network time");
}

/* ------------------------------------------------------------------ boot */

void rtc_keep_boot(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_period = factory_period();
    const keep_slot_t *k = slot_latest();
    if (k) {
        s_seq = k->seq;
        if (k->learned) {                       /* keep the rate learned in earlier sessions */
            s_period = k->period_us;
            s_learned = true;
        }
    }

    ds_probe();

    esp_reset_reason_t rr = esp_reset_reason();
    uint64_t ticks = rtc_time_get();
    if (s_ds_valid) {
        s_boot = BOOT_EXT;
        s_restored = true;
    } else if (time_valid()) {
        s_boot = BOOT_KEPT;                     /* soft restart / OTA: ESP-IDF carried the time over */
    } else if (k && ticks >= k->ticks) {
        double dt_us = (double)(ticks - k->ticks) * k->period_us;
        int64_t t = k->epoch_us + (int64_t)dt_us;
        if (dt_us < 400.0 * 86400e6 && t / 1000000 > VALID_EPOCH) {
            struct timeval tv = {.tv_sec = (time_t)(t / 1000000), .tv_usec = (suseconds_t)(t % 1000000)};
            settimeofday(&tv, NULL);
            s_boot = BOOT_RESTORED;
            s_off_s = (int64_t)(dt_us / 1e6);
            s_restored = true;
            s_err_pending = true;
        } else {
            s_boot = BOOT_LOST_COUNT;
        }
    } else if (k) {
        s_boot = BOOT_LOST_COUNT;
    } else {
        s_boot = nvs_armed() ? BOOT_LOST : BOOT_FIRST;
    }
    ESP_LOGI(TAG, "boot: reset reason %d, rtc ticks %llu, record %s, outcome %d, off %lld s",
             (int)rr, (unsigned long long)ticks, k ? "valid" : "none", (int)s_boot, (long long)s_off_s);

    vbat_apply(g_settings.rtc_vbat);
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    save_locked();
    xSemaphoreGive(s_mtx);
}

void rtc_keep_vbat_changed(void)
{
    if (g_settings.rtc_vbat != s_vbat_on) vbat_apply(g_settings.rtc_vbat);
}

/* ------------------------------------------------------------------ running */

static void keep_task(void *arg)
{
    (void)arg;
    for (;;) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(SAVE_PERIOD_S * 1000));
        xSemaphoreTake(s_mtx, portMAX_DELAY);
        save_locked();
        xSemaphoreGive(s_mtx);
        if (s_ds_present && (s_ds_write_req || (s_ds_last_write && time(NULL) - s_ds_last_write > 6 * 3600))) {
            s_ds_write_req = false;
            ds_write();
        }
    }
}

void rtc_keep_start(void)
{
    if (s_task) return;
    xTaskCreatePinnedToCore(keep_task, "rtc_keep", 3072, NULL, 2, &s_task, 1);
}

void rtc_keep_ntp_synced(void)
{
    if (!s_mtx) return;
    uint64_t t = rtc_time_get();
    int64_t us = now_us();
    xSemaphoreTake(s_mtx, portMAX_DELAY);
    if (s_err_pending) {
        /* compare with what the battery clock would say now (from the last save, <= 30 s old) */
        const keep_slot_t *k = slot_latest();
        if (k && t >= k->ticks) {
            int64_t predicted = k->epoch_us + (int64_t)((double)(t - k->ticks) * k->period_us);
            s_err_s = (double)(us - predicted) / 1e6;
            s_err_valid = true;
            ESP_LOGI(TAG, "battery clock was off by %+.1f s after %lld s without power", s_err_s, (long long)s_off_s);
        }
        s_err_pending = false;
    }
    if (!s_learn0) {
        s_learn0 = true;
        s_l0_us = us;
        s_l0_ticks = t;
    } else if (us - s_l0_us >= 20LL * 60 * 1000000 && t > s_l0_ticks) {
        double p = (double)(us - s_l0_us) / (double)(t - s_l0_ticks);
        double f = factory_period();
        if (p > f * 0.9 && p < f * 1.1) {
            s_period = p;
            s_learned = true;
            ESP_LOGI(TAG, "RTC rate %.1f ppm vs factory calibration (over %lld min)",
                     (p / f - 1.0) * 1e6, (long long)((us - s_l0_us) / 60000000));
        }
    }
    s_last_ntp_us = us;
    s_last_ntp_ticks = t;
    s_restored = false;
    save_locked();
    xSemaphoreGive(s_mtx);
    s_ds_write_req = true;
    if (s_task) xTaskNotifyGive(s_task);
}

bool rtc_keep_restored(void) { return s_restored; }

void rtc_keep_status(char *out, size_t n)
{
    char d[32], line1[200], line2[120] = "";
    if (s_ds_present) {
        snprintf(line1, sizeof(line1), "DS3231 clock module: %s",
                 s_boot == BOOT_EXT ? "time restored from it at power-up" :
                 s_ds_valid ? "set from network time" : "found, waiting for network time to set it");
    } else {
        switch (s_boot) {
        case BOOT_RESTORED:
            fmt_dur(d, sizeof(d), s_off_s);
            if (s_err_valid) {
                char e[32];
                if (s_err_s > -1.0 && s_err_s < 1.0) snprintf(e, sizeof(e), "under 1 s");
                else if (s_err_s > -90.0 && s_err_s < 90.0) snprintf(e, sizeof(e), "%+.0f s", s_err_s);
                else snprintf(e, sizeof(e), "%+.1f min", s_err_s / 60.0);
                snprintf(line1, sizeof(line1), "Restored after %s without power (off by %s)", d, e);
            } else {
                snprintf(line1, sizeof(line1), "Restored after %s without power", d);
            }
            break;
        case BOOT_KEPT: snprintf(line1, sizeof(line1), "Time carried over the last restart"); break;
        case BOOT_LOST_COUNT: snprintf(line1, sizeof(line1), "Last power cut: memory held but the RTC counter restarted"); break;
        case BOOT_LOST: snprintf(line1, sizeof(line1), "Last power cut: nothing was held (check the battery)"); break;
        case BOOT_FIRST: snprintf(line1, sizeof(line1), "Not tested yet - unplug the panel once to try it"); break;
        default: snprintf(line1, sizeof(line1), "-"); break;
        }
    }
    if (!s_vbat_on) {
        snprintf(line2, sizeof(line2), "Battery switch-over is off");
    } else if (s_last_save) {
        long ago = (long)(time(NULL) - s_last_save);
        snprintf(line2, sizeof(line2), "Saved %ld s ago%s", ago < 0 ? 0 : ago,
                 s_learned ? ", rate tuned to network time" : "");
    } else {
        snprintf(line2, sizeof(line2), "Waiting for the time to be set");
    }
    snprintf(out, n, "%s\n%s", line1, line2);
}
