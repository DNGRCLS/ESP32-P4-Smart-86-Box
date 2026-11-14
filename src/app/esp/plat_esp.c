/* platform.h implementation for the ESP32-P4 panel. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "board.h"
#include "esp_app_desc.h"
#include "esp_arduino_version.h"
#include "sdcard.h"
#include "esp_chip_info.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "../../drivers/esp_lvgl_port/esp_lvgl_port.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "hal/efuse_hal.h"
#include "net.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "plat_esp.h"
#include "rtc_keep.h"
#include "../core/platform.h"
#include "../core/settings.h"
#include "../core/tz_table.h"

static const char *TAG = "plat";
static char s_ntp_applied[64];

/* ------------------------------------------------------------------ settings */

void settings_load(void)
{
    settings_set_defaults(&g_settings);
    nvs_handle_t h;
    if (nvs_open("p4box", NVS_READONLY, &h) == ESP_OK) {
        size_t len = 0;
        if (nvs_get_blob(h, "cfg", NULL, &len) == ESP_OK && len > 4) {
            settings_t tmp;
            settings_set_defaults(&tmp);
            /* New fields are only ever appended, so an older, shorter blob still lines up. */
            size_t want = len < sizeof(tmp) ? len : sizeof(tmp);
            uint8_t *buf = malloc(len);
            if (buf && nvs_get_blob(h, "cfg", buf, &len) == ESP_OK) {
                memcpy(&tmp, buf, want);
                if (tmp.version < 2) {   /* saved by 0.1.0: radio fields did not exist yet */
                    tmp.radio_volume = 60;
                    tmp.radio_last = 0;
                    tmp.radio_resume = false;
                    tmp.radio_was_playing = false;
                }
                /* rtc_vbat sits in what was the old struct's tail padding: don't trust it */
                if (tmp.version < 7) tmp.rtc_vbat = true;
                g_settings = tmp;
            }
            free(buf);
        }
        nvs_close(h);
    }
    settings_sanitize(&g_settings);
    ESP_LOGI(TAG, "settings loaded (tz %s, wifi '%s')", g_settings.tz_id, g_settings.wifi_ssid);
}

void plat_settings_save(void)
{
    nvs_handle_t h;
    if (nvs_open("p4box", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", &g_settings, sizeof(g_settings));
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "settings saved");
}

/* ------------------------------------------------------------------ time */

static void sntp_sync_cb(struct timeval *tv)
{
    (void)tv;
    net_set_time_synced();
    rtc_keep_ntp_synced();
    ESP_LOGI(TAG, "time synchronised");
}

void time_service_init(void)
{
    plat_time_apply();
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG_MULTIPLE(2, ESP_SNTP_SERVER_LIST(g_settings.ntp_server, "time.google.com"));
    cfg.sync_cb = sntp_sync_cb;
    cfg.smooth_sync = false;
    snprintf(s_ntp_applied, sizeof(s_ntp_applied), "%s", g_settings.ntp_server);
    esp_netif_sntp_init(&cfg);
}

void plat_time_apply(void)
{
    char tz[96];
    tz_build_posix(&g_settings, tz, sizeof(tz));
    setenv("TZ", tz, 1);
    tzset();
    rtc_keep_vbat_changed();
    if (s_ntp_applied[0] && strcmp(s_ntp_applied, g_settings.ntp_server) != 0) {
        snprintf(s_ntp_applied, sizeof(s_ntp_applied), "%s", g_settings.ntp_server);
        /* esp_sntp keeps a pointer to the name: hand it our persistent copy */
        esp_sntp_setservername(0, s_ntp_applied);
        if (esp_sntp_enabled()) esp_sntp_restart();
    }
}

time_t plat_now(void) { return time(NULL); }

void plat_rtc_status(char *out, size_t n) { rtc_keep_status(out, n); }
bool plat_rtc_restored(void) { return rtc_keep_restored(); }

/* ------------------------------------------------------------------ network */

void plat_net_get_status(net_status_t *st) { net_get_status(st); }

typedef struct {
    wifi_scan_cb_t cb;
    void *user;
} scan_req_t;

static void scan_task(void *arg)
{
    scan_req_t *r = arg;
    wifi_scan_rec_t *recs = calloc(24, sizeof(wifi_scan_rec_t));
    int n = recs ? net_wifi_scan(recs, 24) : 0;
    if (lvgl_port_lock(0)) {
        r->cb(recs, n, r->user);
        lvgl_port_unlock();
    }
    free(recs);
    free(r);
    vTaskDelete(NULL);
}

void plat_wifi_scan_async(wifi_scan_cb_t cb, void *user)
{
    scan_req_t *r = malloc(sizeof(*r));
    r->cb = cb;
    r->user = user;
    if (xTaskCreate(scan_task, "wscan", 6144, r, 3, NULL) != pdPASS) {
        free(r);
        cb(NULL, 0, user);
    }
}

void plat_wifi_connect(const char *ssid, const char *pass)
{
    net_wifi_connect(ssid, pass);
    plat_settings_save();
}

void plat_wifi_forget(void)
{
    net_wifi_forget();
    plat_settings_save();
}

void plat_portal_start(void) { net_portal_start(false); }
void plat_portal_stop(void) { net_portal_stop(); }

/* ------------------------------------------------------------------ system */

void plat_backlight_set(int percent) { board_backlight_set(percent); }

const char *plat_fw_version(void) { return P4BOX_VERSION; }

void plat_sys_info(char *buf, int len)
{
    esp_chip_info_t ci;
    esp_chip_info(&ci);
    unsigned rev = efuse_hal_chip_revision();
    int64_t up = esp_timer_get_time() / 1000000;
    size_t ps = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    size_t in = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    sd_status_t sd;
    plat_sd_status(&sd);
    char card[64];
    if (sd.state == SD_READY)
        snprintf(card, sizeof(card), "%.1f of %.1f GB used", sd.used_bytes / 1e9, sd.total_bytes / 1e9);
    else
        snprintf(card, sizeof(card), "%s", sd.state == SD_MOUNTING ? "reading..." : "not inserted");
    snprintf(buf, len,
             "Firmware   %s  (Arduino core %d.%d.%d, ESP-IDF %s)\n"
             "Board      ESP32-P4-86-Panel-ETH-2RO\n"
             "Chip       ESP32-P4 rev %u.%u, %d cores\n"
             "Memory     %.1f MB PSRAM free, %u KB internal free (lowest %u KB)\n"
             "microSD    %s\n"
             "Wi-Fi co-processor   ESP32-C6, firmware %s\n"
             "Uptime     %lld h %lld min",
             plat_fw_version(), ESP_ARDUINO_VERSION_MAJOR, ESP_ARDUINO_VERSION_MINOR, ESP_ARDUINO_VERSION_PATCH,
             esp_get_idf_version(), rev / 100, rev % 100, ci.cores, ps / 1048576.0, (unsigned)(in / 1024), (unsigned)(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL) / 1024), card,
             net_c6_version(), up / 3600, (up % 3600) / 60);
}

static void reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(600));
    esp_restart();
}

void plat_reboot(void)
{
    plat_settings_save();
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
}

void plat_factory_reset(void)
{
    nvs_handle_t h;
    const char *spaces[] = {"p4box", "p4lock"};
    for (int i = 0; i < 2; i++) {
        if (nvs_open(spaces[i], NVS_READWRITE, &h) == ESP_OK) {
            nvs_erase_all(h);
            nvs_commit(h);
            nvs_close(h);
        }
    }
    board_relay_set(1, false);
    board_relay_set(2, false);
    xTaskCreate(reboot_task, "reboot", 2048, NULL, 5, NULL);
}

/* ------------------------------------------------------------------ radio + stations */

#include "radio.h"
#include "usb_audio.h"
#include "../core/stations.h"

void stations_load(void)
{
    stations_set_defaults(&g_stations);
    nvs_handle_t h;
    if (nvs_open("p4box", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = 0;
    if (nvs_get_blob(h, "stations", NULL, &len) == ESP_OK && len > 2) {
        char *buf = malloc(len + 1);
        if (buf && nvs_get_blob(h, "stations", buf, &len) == ESP_OK) {
            buf[len] = 0;
            stations_t *tmp = malloc(sizeof(stations_t));   /* ~15 KB: never on the stack */
            if (tmp && stations_from_json(tmp, buf)) g_stations = *tmp;
            free(tmp);
        }
        free(buf);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "%d radio stations", g_stations.count);
}

void plat_stations_save(void)
{
    char *js = stations_to_json(&g_stations);
    if (!js) return;
    nvs_handle_t h;
    if (nvs_open("p4box", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "stations", js, strlen(js));
        nvs_commit(h);
        nvs_close(h);
    }
    free(js);
}

void plat_radio_play(const char *name, const char *url)
{
    radio_force_speaker(false);   /* ordinary listening: the chosen output again */
    radio_play(name, url);
    int idx = stations_find_url(&g_stations, url);
    if (idx >= 0) g_settings.radio_last = idx;
    g_settings.radio_was_playing = true;
    snprintf(g_settings.last_play_name, sizeof(g_settings.last_play_name), "%s", name ? name : "");
    snprintf(g_settings.last_play_url, sizeof(g_settings.last_play_url), "%s", url);
    plat_settings_save();
}

void plat_alarm_radio(const char *name, const char *url)
{
    radio_force_speaker(true);    /* wake-up on the built-in speaker, never on headphones */
    radio_play(name, url);
}

void plat_alarm_speaker(bool on) { radio_force_speaker(on); }

void plat_audio_out_get(audio_out_t *o)
{
    memset(o, 0, sizeof(*o));
    o->connected = usb_audio_connected();
    o->active = radio_output_is_usb();
    o->prefer_usb = g_settings.radio_out_usb;
    usb_audio_name(o->name, sizeof(o->name));
}

void plat_audio_out_prefer_usb(bool on)
{
    g_settings.radio_out_usb = on;
    radio_output_usb(on);
    plat_settings_save();
}

void plat_alarm_radio_stop(void)
{
    radio_stop();
    radio_force_speaker(false);
    if (g_settings.radio_was_playing) {
        g_settings.radio_was_playing = false;
        plat_settings_save();
    }
}

void plat_alarm_tone(int kind) { radio_buzzer(kind); }
void plat_alarm_volume(int percent) { radio_set_volume(percent); }

void plat_radio_stop(void)
{
    radio_stop();
    g_settings.radio_was_playing = false;
    plat_settings_save();
}

void plat_radio_set_volume(int percent)
{
    radio_set_volume(percent);
    g_settings.radio_volume = (uint8_t)percent;
}

void plat_radio_get_status(radio_status_t *st) { radio_get_status(st); }

void plat_radio_search(const char *query, radio_search_cb_t cb, void *user) { radio_search(query, cb, user); }

void radio_boot(void)
{
    radio_output_usb(g_settings.radio_out_usb);
    usb_audio_init();
    radio_init();
    radio_set_volume(g_settings.radio_volume);
    int i = g_settings.radio_last;
    if (g_settings.radio_resume && g_settings.radio_was_playing && i >= 0 && i < g_stations.count) {
        ESP_LOGI(TAG, "resuming %s", g_stations.items[i].name);
        radio_play(g_stations.items[i].name, g_stations.items[i].url);
    }
}

/* ------------------------------------------------------------------ lock */

#include "esp_random.h"
#include "../core/lockcore.h"

void plat_relay_set(int relay, bool on) { board_relay_set(relay, on); }
void plat_random(void *buf, int len) { esp_fill_random(buf, len); }
uint32_t plat_uptime_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

void plat_lock_save(void)
{
    nvs_handle_t h;
    if (nvs_open("p4lock", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "cfg", &g_lock, sizeof(g_lock));
    nvs_commit(h);
    nvs_close(h);
}

void plat_lock_log_save(void)
{
    nvs_handle_t h;
    if (nvs_open("p4lock", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "log", g_lock_log, sizeof(lock_log_t) * g_lock_log_count);
    nvs_commit(h);
    nvs_close(h);
}

void lock_load(void)
{
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open("p4lock", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(g_lock);
        ok = nvs_get_blob(h, "cfg", &g_lock, &len) == ESP_OK && len == sizeof(g_lock);
        size_t ll = sizeof(g_lock_log);
        if (nvs_get_blob(h, "log", g_lock_log, &ll) == ESP_OK) g_lock_log_count = ll / sizeof(lock_log_t);
        nvs_close(h);
    }
    if (!ok) {
        lock_set_defaults(&g_lock);
        plat_lock_save();
    }
    lock_restore();
    ESP_LOGI(TAG, "lock: relay1 %s, relay2 %s, admin code %s", g_lock.relay1_unlocked ? "UNLOCKED" : "locked",
             g_lock.relay2_on ? "on" : "off", g_lock.admin_set ? "set" : "not set");
}
