#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "weather.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SETTINGS_VERSION 7

typedef enum { DST_AUTO = 0, DST_ALWAYS_STANDARD = 1, DST_ALWAYS_DAYLIGHT = 2 } dst_mode_t;
typedef enum { CLOCK_FACE_DIGITAL = 0, CLOCK_FACE_ANALOG = 1 } clock_face_t;

#define ALARM_MAX 6
typedef enum { ALARM_SND_BEEP = 0, ALARM_SND_BUZZ = 1, ALARM_SND_RADIO = 2 } alarm_sound_t;

typedef struct {
    bool used;                   /* slot holds an alarm */
    bool on;                     /* armed */
    uint8_t hour, minute;        /* 0..23, 0..59 local time */
    uint8_t days;                /* bit 0 = Sunday ... bit 6 = Saturday; 0 = once (disarms after ringing) */
    uint8_t sound;               /* alarm_sound_t */
    char label[24];
} alarm_t;

typedef struct {
    uint16_t version;

    /* Network */
    char wifi_ssid[33];
    char wifi_pass[65];
    char hostname[32];

    /* Display */
    uint8_t brightness;          /* 5..100 % */
    uint16_t screensaver_s;      /* 0 = off */
    bool night_dim;
    uint8_t night_start_h;       /* 0..23 */
    uint8_t night_end_h;         /* 0..23 */
    uint8_t night_brightness;    /* 1..100 % */

    /* Time */
    char tz_id[40];              /* key into tz table, e.g. "ca_toronto" */
    uint8_t dst_mode;            /* dst_mode_t */
    char tz_custom[64];          /* optional POSIX TZ override, empty = unused */
    bool clock_24h;
    bool clock_seconds;
    uint8_t clock_face;          /* clock_face_t */
    char ntp_server[64];

    /* Calendar (used by later apps) */
    char province[4];            /* "ON", "QC", ... */

    /* ---- added in 0.2.0 (new fields are only ever appended) ---- */
    uint8_t radio_volume;        /* 0..100 */
    int16_t radio_last;          /* station index last played, -1 none */
    bool radio_resume;           /* start the last station after a restart */
    bool radio_was_playing;      /* radio was on when settings were last saved */

    /* ---- added in 1.0.0 (Photos app) ---- */
    uint8_t saver_mode;          /* saver_mode_t */
    uint8_t photo_interval_s;    /* slideshow / photo frame interval, 3..250 s */
    bool photo_shuffle;          /* random order */
    bool photo_fill;             /* crop to fill the screen instead of showing the whole photo */
    bool photo_recursive;        /* slideshow and photo frame include subfolders */
    bool photo_frame_clock;      /* show the time over the photo frame */
    char photo_dir[160];         /* photo frame folder, relative to the card root ("" = whole card) */

    /* ---- added in 1.1.0 (alarms) ---- */
    alarm_t alarms[ALARM_MAX];
    uint8_t alarm_snooze_min;    /* 1..10 */
    uint8_t alarm_volume;        /* 10..100 % */
    bool alarm_fade;             /* volume rises gently over the first 30 s */
    uint8_t alarm_ring_min;      /* stops by itself after this many minutes (5..60) */
    char last_play_name[64];     /* last station played, also from search results: used for wake-to-radio */
    char last_play_url[256];

    /* ---- added in 1.2.0 (weather) ---- */
    char wx_key[48];             /* OpenWeather API key */
    uint8_t wx_units;            /* wx_units_t */
    uint8_t wx_aq_scale;         /* wx_aq_scale_t */
    bool wx_home;                /* current weather on the home screen */
    uint8_t wx_loc_count;
    uint8_t wx_loc_sel;          /* location shown */
    wx_loc_t wx_locs[WX_LOC_MAX];

    /* ---- added in 1.3.0 (USB / Bluetooth audio) ---- */
    bool radio_out_usb;          /* radio plays on a USB audio device (Bluetooth transmitter) when plugged in */

    /* ---- added in 1.4.0 (battery-backed clock) ---- */
    bool rtc_vbat;               /* switch the chip's RTC onto the backup battery when main power drops */
} settings_t;

typedef enum { WX_METRIC = 0, WX_IMPERIAL = 1 } wx_units_t;
typedef enum { WX_AQ_AQHI = 0, WX_AQ_US = 1, WX_AQ_OWM = 2 } wx_aq_scale_t;

typedef enum { SAVER_CLOCK = 0, SAVER_PHOTOS = 1 } saver_mode_t;

extern settings_t g_settings;

void settings_set_defaults(settings_t *s);
/* Clamp/repair fields after loading or editing. */
void settings_sanitize(settings_t *s);

/* JSON helpers used by the web API. Caller frees returned string with free(). */
char *settings_to_json(const settings_t *s);
/* Applies the keys present in json onto s. Returns true if anything changed. */
bool settings_apply_json(settings_t *s, const char *json, bool *wifi_changed);

#ifdef __cplusplus
}
#endif
