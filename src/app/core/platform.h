#pragma once
/*
 * Interface between the portable UI (LVGL only) and the hardware/OS layer.
 * Implemented by main/esp/ on the device and by sim/ on a Linux host.
 *
 * Threading rule: every function here may be called from the LVGL task with the
 * LVGL lock held.  Callbacks marked "LVGL context" are invoked with the lock held.
 */
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "weather.h"

#define P4BOX_VERSION "1.4.0"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum { NET_ACTIVE_NONE = 0, NET_ACTIVE_ETH, NET_ACTIVE_WIFI } net_active_t;

typedef struct {
    bool eth_link;          /* cable plugged and link up */
    bool eth_has_ip;
    char eth_ip[16];
    bool wifi_configured;   /* SSID stored */
    bool wifi_connected;
    bool wifi_connecting;
    char wifi_ssid[33];
    char wifi_ip[16];
    int wifi_rssi;
    bool portal_active;     /* setup hotspot running */
    char portal_ssid[33];
    char portal_ip[16];
    net_active_t active;    /* interface carrying the default route */
    bool time_synced;       /* SNTP succeeded at least once (or RTC held a valid time) */
    time_t last_sync;
} net_status_t;

typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} wifi_scan_rec_t;

typedef void (*wifi_scan_cb_t)(const wifi_scan_rec_t *recs, int count, void *user); /* LVGL context */

void plat_net_get_status(net_status_t *st);
void plat_wifi_scan_async(wifi_scan_cb_t cb, void *user);
void plat_wifi_connect(const char *ssid, const char *pass); /* stores credentials in g_settings */
void plat_wifi_forget(void);
void plat_portal_start(void);
void plat_portal_stop(void);

void plat_backlight_set(int percent);

/* Persist g_settings (debounced by caller). */
void plat_settings_save(void);
/* Re-apply time zone / NTP server after a settings change. */
void plat_time_apply(void);
/* Battery-backed clock: status text (two lines) and whether the time came from it. */
void plat_rtc_status(char *out, size_t n);
bool plat_rtc_restored(void);

time_t plat_now(void);

/* ---- Radio ---- */
typedef enum {
    RADIO_IDLE = 0,
    RADIO_CONNECTING,
    RADIO_BUFFERING,
    RADIO_PLAYING,
    RADIO_RECONNECTING,
    RADIO_ERROR,
} radio_state_t;

typedef struct {
    radio_state_t state;
    char name[64];          /* station being played */
    char url[256];
    char title[160];        /* "Artist - Song" from the stream, if sent */
    char codec[12];         /* "MP3", "AAC", ... */
    int bitrate_kbps;
    int sample_rate;
    int channels;
    int buffer_pct;
    char error[96];
} radio_status_t;

typedef struct {
    char name[64];
    char url[256];
    char country[4];
    char codec[8];
    int bitrate;
    char tags[48];
} radio_search_rec_t;

typedef void (*radio_search_cb_t)(const radio_search_rec_t *recs, int count, const char *error,
                                  void *user);   /* LVGL context */

void plat_radio_play(const char *name, const char *url);
void plat_radio_stop(void);
void plat_radio_set_volume(int percent);
void plat_radio_get_status(radio_status_t *st);
void plat_radio_search(const char *query, radio_search_cb_t cb, void *user);
void plat_stations_save(void);

/* ---- Audio output: built-in speaker or a USB audio device (e.g. a USB Bluetooth transmitter) ---- */
typedef struct {
    bool connected;          /* a USB Audio 1.0 output device is plugged in */
    bool active;             /* the radio is playing on it right now */
    bool prefer_usb;         /* setting: use it when plugged in */
    char name[48];           /* product name the device reports */
} audio_out_t;
void plat_audio_out_get(audio_out_t *o);
void plat_audio_out_prefer_usb(bool on);                    /* saves the setting */

/* ---- Alarm audio: same speaker as the radio, but nothing here changes the saved radio state ---- */
void plat_alarm_radio(const char *name, const char *url);   /* start a stream for wake-to-radio */
void plat_alarm_radio_stop(void);
void plat_alarm_tone(int kind);                             /* 0 off, 1 beeps, 2 buzzer */
void plat_alarm_volume(int percent);                        /* temporary speaker volume */
void plat_alarm_speaker(bool on);                           /* alarms always play on the built-in speaker */

/* ---- Speed test (Cloudflare) ---- */
typedef enum { SPD_IDLE = 0, SPD_PREPARE, SPD_PING, SPD_DOWNLOAD, SPD_UPLOAD, SPD_DONE, SPD_ERROR } spd_phase_t;

typedef struct {
    spd_phase_t phase;
    float ping_ms, jitter_ms, down_mbps, up_mbps;
    float live_mbps;          /* current throughput during download/upload */
    int progress_pct;         /* within the current phase */
    char iface[12];           /* "Ethernet" / "Wi-Fi" */
    char server[40];          /* Cloudflare location, e.g. "YYZ" */
    char ip[48];              /* public IP seen by Cloudflare */
    char error[80];
} speedtest_t;

typedef struct {
    time_t t;
    float ping_ms, jitter_ms, down_mbps, up_mbps;
    char iface[12];
} speed_hist_t;

void plat_speedtest_start(void);
void plat_speedtest_cancel(void);
void plat_speedtest_get(speedtest_t *st);
int plat_speed_history(speed_hist_t *out, int max);   /* newest first */
/* ---- Photos (microSD card, mounted at /sdcard) ---- */
typedef enum { SD_NONE = 0, SD_MOUNTING, SD_READY, SD_ERROR } sd_state_t;

typedef struct {
    sd_state_t state;
    uint64_t total_bytes, used_bytes;
    char error[64];
} sd_status_t;

typedef struct {
    char name[96];          /* file or folder name (no path) */
    bool is_dir;
    uint32_t size;
} photo_entry_t;

typedef struct {
    uint16_t *pixels;       /* RGB565, w * h, owned by the receiver: release with plat_photo_free() */
    int w, h;               /* size of the pixel buffer */
    int src_w, src_h;       /* photo size as stored (after EXIF rotation) */
    char error[64];         /* set when pixels == NULL */
} photo_img_t;

typedef enum { PHOTO_FIT = 0, PHOTO_FILL = 1 } photo_fit_t;
/* Decoder queues: thumbnails and full-screen images are cancelled independently. */
typedef enum { PHOTO_Q_THUMB = 0, PHOTO_Q_VIEW = 1 } photo_queue_t;

/* LVGL context.  Not called at all if the queue was cancelled after the request was made. */
typedef void (*photo_cb_t)(const photo_img_t *img, void *user);

void plat_sd_status(sd_status_t *st);
void plat_sd_remount(void);
/* Lists one folder (path relative to the card root, "" = root): sub-folders first, then .jpg/.jpeg
 * files, each group sorted by name.  Returns the number of entries, or -1 if the card is not ready. */
int plat_photo_list(const char *dir, photo_entry_t *out, int max);
/* All JPEG paths (relative to the card root) below dir, optionally including sub-folders.
 * Returns a malloc'ed array of malloc'ed strings (free with plat_photo_paths_free); *count is set. */
char **plat_photo_paths(const char *dir, bool recursive, int max, int *count);
void plat_photo_paths_free(char **paths, int count);
/* Decode a JPEG into a w x h box (PHOTO_FIT: whole photo fits inside, PHOTO_FILL: covers the box,
 * centre-cropped).  EXIF orientation is applied.  Runs on a worker task; cb fires in LVGL context. */
void plat_photo_decode(photo_queue_t q, const char *path, int w, int h, photo_fit_t fit, bool thumbnail,
                       photo_cb_t cb, void *user);
void plat_photo_cancel(photo_queue_t q);
void plat_photo_free(uint16_t *pixels);
void plat_photo_clear_cache(void);

/* ---- Wi-Fi co-processor (ESP32-C6, ESP-Hosted) firmware ---- */
typedef struct {
    bool link_ok;            /* the SDIO link to the C6 came up */
    bool known;              /* versions could be read (very old C6 firmware cannot report one) */
    bool update_available;   /* the C6 runs older ESP-Hosted firmware than this build expects */
    bool running, done, failed;
    int pct;
    char installed[16], expected[16];
    char msg[80];
} c6fw_t;
void plat_c6_fw_status(c6fw_t *st);
void plat_c6_fw_update(void);   /* downloads from Espressif and flashes the C6, then restarts */

/* ---- Weather (OpenWeather): the location shown is g_settings.wx_locs[wx_loc_sel] ---- */
void plat_weather_get(const wx_loc_t *loc, weather_t *out);   /* cached data for that place (valid = false: none) */
uint32_t plat_weather_gen(void);                              /* changes whenever cached data or the busy flag change */
bool plat_weather_busy(void);
void plat_weather_refresh(bool force);                        /* refresh the location shown (async) */
void plat_weather_key_changed(void);                          /* key changed: forget everything, check One Call again */
const char *plat_weather_onecall(void);                       /* "One Call 4.0", "not on this key", ... */
typedef void (*wx_search_cb_t)(const wx_loc_t *res, int n, const char *err, void *user);   /* LVGL context */
void plat_weather_search(const char *query, wx_search_cb_t cb, void *user);

const char *plat_fw_version(void);
void plat_sys_info(char *buf, int len);   /* multi-line human readable text */
void plat_reboot(void);
void plat_factory_reset(void);

#ifdef __cplusplus
}
#endif
