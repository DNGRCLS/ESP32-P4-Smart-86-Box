#pragma once
#include <stdbool.h>
#include <time.h>
#include "../../lvgl/lvgl.h"
#include "ui.h"
#include "../core/settings.h"
#include "../core/platform.h"
#include "../assets/assets.h"

/* ---------- Palette ---------- */
#define UI_C_BG        lv_color_hex(0x0B0E13)
#define UI_C_SURFACE   lv_color_hex(0x161B22)
#define UI_C_SURFACE2  lv_color_hex(0x212833)
#define UI_C_BORDER    lv_color_hex(0x2D3540)
#define UI_C_TEXT      lv_color_hex(0xE8EDF3)
#define UI_C_MUTED     lv_color_hex(0x8A96A6)
#define UI_C_ACCENT    lv_color_hex(0x3D8BFF)
#define UI_C_ACCENT2   lv_color_hex(0xFF8A3D)
#define UI_C_OK        lv_color_hex(0x3FB950)
#define UI_C_WARN      lv_color_hex(0xE3B341)
#define UI_C_ERR       lv_color_hex(0xF85149)

#define UI_W 720
#define UI_H 720
#define UI_BAR_H 68

/* ---------- Fonts ---------- */
typedef enum { UI_FW_LIGHT = 0, UI_FW_REGULAR, UI_FW_MEDIUM, UI_FW_BOLD } ui_fw_t;
/* Large text in Poppins rendered by tiny_ttf (cached per weight/size).  No LV_SYMBOL glyphs. */
const lv_font_t *ui_font(ui_fw_t weight, int px);

/* ---------- Apps ---------- */
typedef enum {
    APP_CLOCK = 0,
    APP_WEATHER,
    APP_RADIO,
    APP_LOCK,
    APP_CALENDAR,
    APP_SPEEDTEST,
    APP_PHOTOS,
    APP_SETTINGS,
    APP_COUNT
} app_id_t;

typedef struct {
    const char *name;
    const lv_image_dsc_t *icon;
    void (*create)(lv_obj_t *parent);  /* build UI inside the content area */
    void (*destroy)(void);             /* optional: stop timers/tasks before the UI is deleted */
    void (*on_settings)(void);         /* optional: settings changed elsewhere */
} ui_app_t;

void ui_open_app(app_id_t id);
void ui_go_home(void);
lv_obj_t *ui_content(void);

/* Settings were modified by the UI: persist (debounced) and apply side effects. */
void ui_settings_changed(void);
/* Show a brightness level for a few seconds (live preview while dragging a slider). */
void ui_backlight_preview(int percent);

/* ---------- Shared helpers ---------- */
void ui_fmt_time(char *buf, size_t n, const struct tm *t, bool seconds, bool with_ampm);
void ui_fmt_hour(char *buf, size_t n, int hour);  /* "10 PM" or "22:00" per 12/24h setting */
long ui_utc_offset(const struct tm *local);       /* seconds east of UTC for a localtime_r() result */
void ui_fmt_utc_offset(char *buf, size_t n, const struct tm *local);  /* "UTC-04:00" */
lv_obj_t *ui_card(lv_obj_t *parent);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t color);
lv_obj_t *ui_section_title(lv_obj_t *parent, const char *text);
lv_obj_t *ui_row(lv_obj_t *parent, const char *label);  /* returns the row; add a widget to it */

/* Modal dialogs (ui_dialogs.c) */
typedef void (*ui_text_cb_t)(const char *text, void *user);
void ui_text_prompt(const char *title, const char *initial, bool password, int max_len,
                    ui_text_cb_t on_ok, void *user);
typedef void (*ui_confirm_cb_t)(void *user);
void ui_confirm(const char *title, const char *message, const char *ok_text, lv_color_t ok_color,
                ui_confirm_cb_t on_ok, void *user);
void ui_toast(const char *text);
typedef void (*ui_key_cb_t)(char key, void *user);   /* '0'..'9', '<' backspace, '#' OK */
lv_obj_t *ui_keypad_create(lv_obj_t *parent, int w, int h, ui_key_cb_t cb, void *user);
void ui_pin_prompt(const char *title, const char *subtitle, int min_len, int max_len, ui_text_cb_t on_ok, void *user);
typedef void (*ui_menu_cb_t)(int index, void *user);
void ui_menu(const char *title, const char *const *items, int count, ui_menu_cb_t on_pick, void *user);

/* Alarms (alarm.c) */
#include <stddef.h>
void alarm_tick(void);                                   /* once a second */
void alarm_settings_changed(void);
bool alarm_valid_time(void);
bool alarm_ringing(void);
bool alarm_snoozed(time_t *until);
bool alarm_any_on(void);
bool alarm_next(time_t *when, int *slot);                /* next armed alarm */
void alarm_status_text(char *buf, size_t n);             /* "6:30 AM", "Tue 6:30 AM", "Snoozed until ..." or "" */
void alarm_fmt_when(char *buf, size_t n, time_t when);
void alarm_fmt_time(char *buf, size_t n, int hour, int minute);
void alarm_fmt_days(const alarm_t *a, char *buf, size_t n);
const char *alarm_sound_name(int snd);
bool alarm_station(const char **name, const char **url);
void alarm_stop(void);
void alarm_snooze(void);
void alarm_snooze_cancel(void);
int alarm_snooze_slot(void);                             /* -1 when not snoozing */
void alarm_test(const alarm_t *a);                       /* ring now with these settings */
/* ui_core.c */
void ui_wake(void);                                      /* close the screensaver, full brightness */
void ui_refresh_alarm_icons(void);

/* Alarms page inside the Clock app (app_alarms.c) */
void alarms_page_create(lv_obj_t *parent, void (*on_done)(void));
void alarms_page_destroy(void);
void alarms_page_refresh(void);

/* Weather (app_weather.c) */
const lv_image_dsc_t *wx_icon_img(const char *code, bool large);
bool wx_home_summary(const lv_image_dsc_t **icon, char *text, size_t n);

/* Clock face widget (clock_face.c) */
lv_obj_t *clock_face_create(lv_obj_t *parent, bool fullscreen);
void clock_face_rebuild(lv_obj_t *face);   /* after face/12h/seconds settings change */

/* Photos (app_photos.c): full-screen slideshow used by the screensaver.  Returns NULL when there
 * is no card or no photos, so the caller can fall back to the clock. */
lv_obj_t *photo_frame_create(lv_obj_t *parent);
bool photo_viewer_open(void);

/* App descriptors */
extern const ui_app_t app_clock;
extern const ui_app_t app_weather;
extern const ui_app_t app_settings;
extern const ui_app_t app_radio;
extern const ui_app_t app_lock;
extern const ui_app_t app_calendar;
extern const ui_app_t app_speedtest;
extern const ui_app_t app_photos;
