/*
 * UI core: theme, fonts, status bar, launcher, app navigation,
 * screensaver and night dimming.
 */
#include <stdio.h>
#include <string.h>
#include "ui_internal.h"
#include "../core/lockcore.h"

/* ------------------------------------------------------------------ fonts */

typedef struct {
    ui_fw_t w;
    int px;
    lv_font_t *font;
} font_slot_t;

static font_slot_t s_fonts[64];
static int s_font_count;

const lv_font_t *ui_font(ui_fw_t weight, int px)
{
    for (int i = 0; i < s_font_count; i++) {
        if (s_fonts[i].w == weight && s_fonts[i].px == px) return s_fonts[i].font;
    }
    const uint8_t *data;
    size_t len;
    switch (weight) {
    case UI_FW_LIGHT: data = ttf_poppins_light; len = ttf_poppins_light_len; break;
    case UI_FW_MEDIUM: data = ttf_poppins_medium; len = ttf_poppins_medium_len; break;
    case UI_FW_BOLD: data = ttf_poppins_bold; len = ttf_poppins_bold_len; break;
    default: data = ttf_poppins_regular; len = ttf_poppins_regular_len; break;
    }
    lv_font_t *f = lv_tiny_ttf_create_data_ex(data, len, px, LV_FONT_KERNING_NORMAL, 96);
    if (!f) return &lv_font_montserrat_28;
    f->fallback = &lv_font_montserrat_28;
    if (s_font_count < (int)(sizeof(s_fonts) / sizeof(s_fonts[0]))) {
        s_fonts[s_font_count++] = (font_slot_t){weight, px, f};
    }
    return f;
}

/* ------------------------------------------------------------------ app table */

static const ui_app_t *const s_apps[APP_COUNT] = {
    [APP_CLOCK] = &app_clock,
    [APP_WEATHER] = &app_weather,
    [APP_RADIO] = &app_radio,
    [APP_LOCK] = &app_lock,
    [APP_CALENDAR] = &app_calendar,
    [APP_SPEEDTEST] = &app_speedtest,
    [APP_PHOTOS] = &app_photos,
    [APP_SETTINGS] = &app_settings,
};

/* ------------------------------------------------------------------ state */

static lv_obj_t *s_scr;
static lv_obj_t *s_bar, *s_back, *s_title, *s_bar_time, *s_net_icon, *s_net_label, *s_audio_lbl, *s_alarm_lbl;
static lv_obj_t *s_saver_np;     /* now-playing line on the screensaver */
static lv_obj_t *s_saver_alarm;  /* next alarm on the screensaver */
static lv_obj_t *s_home_alarm;   /* next alarm on the home screen */
static lv_obj_t *s_home_wx, *s_home_wx_icon, *s_home_wx_lbl;   /* current weather on the home screen */
static lv_obj_t *s_content;
static int s_current = -1;       /* -1 = launcher */
static lv_obj_t *s_home_time, *s_home_date;

static lv_obj_t *s_saver;        /* screensaver overlay */
static lv_timer_t *s_save_timer;
static int s_applied_bl = -1;
static int s_preview_bl = -1;
static uint32_t s_preview_until;

lv_obj_t *ui_content(void) { return s_content; }

/* ------------------------------------------------------------------ helpers */

void ui_fmt_time(char *buf, size_t n, const struct tm *t, bool seconds, bool with_ampm)
{
    if (g_settings.clock_24h) {
        if (seconds) snprintf(buf, n, "%02d:%02d:%02d", t->tm_hour, t->tm_min, t->tm_sec);
        else snprintf(buf, n, "%02d:%02d", t->tm_hour, t->tm_min);
    } else {
        int h = t->tm_hour % 12;
        if (h == 0) h = 12;
        const char *ap = t->tm_hour < 12 ? "AM" : "PM";
        if (seconds) snprintf(buf, n, "%d:%02d:%02d%s%s", h, t->tm_min, t->tm_sec, with_ampm ? " " : "", with_ampm ? ap : "");
        else snprintf(buf, n, "%d:%02d%s%s", h, t->tm_min, with_ampm ? " " : "", with_ampm ? ap : "");
    }
}

/* days since 1970-01-01 for a proleptic Gregorian date (Howard Hinnant's algorithm) */
static long days_from_civil(long y, unsigned m, unsigned d)
{
    y -= m <= 2;
    long era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}

long ui_utc_offset(const struct tm *lt)
{
    /* Re-derive the epoch time of this local wall-clock as if it were UTC,
       then compare with the real epoch time obtained via mktime(). */
    struct tm copy = *lt;
    time_t real = mktime(&copy);
    long long as_utc = (long long)days_from_civil(lt->tm_year + 1900, lt->tm_mon + 1, lt->tm_mday) * 86400LL +
                       lt->tm_hour * 3600 + lt->tm_min * 60 + lt->tm_sec;
    return (long)(as_utc - (long long)real);
}

void ui_fmt_utc_offset(char *buf, size_t n, const struct tm *lt)
{
    long off = ui_utc_offset(lt);
    char sign = off < 0 ? '-' : '+';
    if (off < 0) off = -off;
    snprintf(buf, n, "UTC%c%02ld:%02ld", sign, off / 3600, (off % 3600) / 60);
}

void ui_fmt_hour(char *buf, size_t n, int hour)
{
    if (g_settings.clock_24h) {
        snprintf(buf, n, "%02d:00", hour);
    } else {
        int h = hour % 12;
        if (h == 0) h = 12;
        snprintf(buf, n, "%d %s", h, hour < 12 ? "AM" : "PM");
    }
}

lv_obj_t *ui_card(lv_obj_t *parent)
{
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_width(c, lv_pct(100));
    lv_obj_set_height(c, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, UI_C_SURFACE, 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, UI_C_BORDER, 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_pad_all(c, 18, 0);
    lv_obj_set_style_pad_row(c, 14, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_height(b, 60);
    lv_obj_set_style_bg_color(b, color, 0);
    lv_obj_set_style_radius(b, 14, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_pad_hor(b, 22, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    return b;
}

lv_obj_t *ui_section_title(lv_obj_t *parent, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(l, UI_C_MUTED, 0);
    lv_obj_set_style_text_letter_space(l, 1, 0);
    return l;
}

lv_obj_t *ui_row(lv_obj_t *parent, const char *label)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_width(r, lv_pct(100));
    lv_obj_set_height(r, LV_SIZE_CONTENT);
    lv_obj_set_style_min_height(r, 56, 0);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 16, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(r);
    lv_label_set_text(l, label);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_set_flex_grow(l, 1);
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    return r;
}

/* ------------------------------------------------------------------ settings & backlight */

static void save_timer_cb(lv_timer_t *t)
{
    lv_timer_pause(t);          /* one save per burst of changes */
    plat_settings_save();
}

#define NIGHT_IDLE_MS 30000u

static bool is_night(int hour)
{
    int a = g_settings.night_start_h, b = g_settings.night_end_h;
    if (a == b) return false;
    if (a < b) return hour >= a && hour < b;
    return hour >= a || hour < b;
}

static void apply_backlight(bool force)
{
    int target = g_settings.brightness;
    if (s_preview_bl >= 0 && lv_tick_get() < s_preview_until) {
        target = s_preview_bl;
    } else {
        s_preview_bl = -1;
        if (g_settings.night_dim) {
            /* Night level only while nobody is using the panel: any touch brings it straight back
             * to normal brightness, and it dims again after NIGHT_IDLE_MS without a touch. */
            time_t now = plat_now();
            struct tm t;
            localtime_r(&now, &t);
            bool idle = s_saver || lv_display_get_inactive_time(NULL) > NIGHT_IDLE_MS;
            if (alarm_ringing()) idle = false;   /* a ringing alarm always gets the full brightness */
            if (idle && is_night(t.tm_hour) && g_settings.night_brightness < target) target = g_settings.night_brightness;
        }
    }
    if (force || target != s_applied_bl) {
        s_applied_bl = target;
        plat_backlight_set(target);
    }
}

void ui_backlight_preview(int percent)
{
    s_preview_bl = percent;
    s_preview_until = lv_tick_get() + 4000;
    apply_backlight(false);
}

static void notify_apps_settings(void)
{
    if (s_current >= 0 && s_apps[s_current]->on_settings) s_apps[s_current]->on_settings();
    if (s_saver) {
        lv_obj_t *face = lv_obj_get_child(s_saver, 0);
        if (face) clock_face_rebuild(face);
    }
}

static void update_bar(void);
static void update_home_clock(void);

void ui_settings_changed(void)
{
    update_bar();
    update_home_clock();
    if (s_save_timer) {
        lv_timer_reset(s_save_timer);
        lv_timer_resume(s_save_timer);
    }
    plat_time_apply();
    apply_backlight(false);
}

void ui_on_external_settings_change(void)
{
    alarm_settings_changed();
    update_bar();
    update_home_clock();
    plat_time_apply();
    apply_backlight(true);
    notify_apps_settings();
}

/* ------------------------------------------------------------------ status bar */

static void back_cb(lv_event_t *e)
{
    (void)e;
    ui_go_home();
}

static void build_bar(void)
{
    s_bar = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, UI_W, UI_BAR_H);
    lv_obj_set_style_bg_color(s_bar, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_side(s_bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(s_bar, 1, 0);
    lv_obj_set_style_border_color(s_bar, UI_C_SURFACE2, 0);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    s_back = lv_button_create(s_bar);
    lv_obj_set_size(s_back, 120, 52);
    lv_obj_align(s_back, LV_ALIGN_LEFT_MID, 10, 0);
    lv_obj_set_style_bg_color(s_back, UI_C_SURFACE2, 0);
    lv_obj_set_style_radius(s_back, 26, 0);
    lv_obj_set_style_shadow_width(s_back, 0, 0);
    lv_obj_set_ext_click_area(s_back, 12);
    lv_obj_t *bl = lv_label_create(s_back);
    lv_label_set_text(bl, LV_SYMBOL_LEFT "  Home");
    lv_obj_set_style_text_font(bl, &lv_font_montserrat_20, 0);
    lv_obj_center(bl);
    lv_obj_add_event_cb(s_back, back_cb, LV_EVENT_CLICKED, NULL);

    s_title = lv_label_create(s_bar);
    lv_obj_set_style_text_font(s_title, ui_font(UI_FW_MEDIUM, 28), 0);
    lv_obj_set_style_text_color(s_title, UI_C_TEXT, 0);
    lv_obj_align(s_title, LV_ALIGN_CENTER, 0, 2);

    s_bar_time = lv_label_create(s_bar);
    lv_obj_set_style_text_font(s_bar_time, ui_font(UI_FW_MEDIUM, 24), 0);
    lv_obj_set_style_text_color(s_bar_time, UI_C_TEXT, 0);
    lv_obj_align(s_bar_time, LV_ALIGN_RIGHT_MID, -20, 2);

    s_net_icon = lv_image_create(s_bar);
    lv_image_set_src(s_net_icon, &icon_ethernet);
    lv_obj_set_style_image_recolor(s_net_icon, UI_C_TEXT, 0);
    lv_obj_set_style_image_recolor_opa(s_net_icon, LV_OPA_COVER, 0);

    s_net_label = lv_label_create(s_bar);
    lv_obj_set_style_text_font(s_net_label, &lv_font_montserrat_24, 0);

    s_audio_lbl = lv_label_create(s_bar);
    lv_label_set_text(s_audio_lbl, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_font(s_audio_lbl, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_audio_lbl, UI_C_ACCENT, 0);
    lv_obj_add_flag(s_audio_lbl, LV_OBJ_FLAG_HIDDEN);

    s_alarm_lbl = lv_label_create(s_bar);
    lv_label_set_text(s_alarm_lbl, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(s_alarm_lbl, &lv_font_montserrat_24, 0);
    lv_obj_add_flag(s_alarm_lbl, LV_OBJ_FLAG_HIDDEN);
}

static void update_bar(void)
{
    time_t now = plat_now();
    struct tm t;
    localtime_r(&now, &t);
    char buf[32];
    ui_fmt_time(buf, sizeof(buf), &t, false, true);
    lv_label_set_text(s_bar_time, buf);

    net_status_t st;
    plat_net_get_status(&st);
    lv_obj_add_flag(s_net_icon, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_net_label, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(s_bar_time);
    if (st.active == NET_ACTIVE_ETH) {
        lv_obj_remove_flag(s_net_icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align_to(s_net_icon, s_bar_time, LV_ALIGN_OUT_LEFT_MID, -14, -2);
    } else {
        const char *sym = LV_SYMBOL_WIFI;
        lv_color_t c = UI_C_TEXT;
        if (st.portal_active && !st.wifi_connected) { sym = LV_SYMBOL_WIFI " AP"; c = UI_C_WARN; }
        else if (!st.wifi_connected) c = UI_C_ERR;
        else if (st.wifi_rssi < -75) c = UI_C_WARN;
        lv_label_set_text(s_net_label, sym);
        lv_obj_set_style_text_color(s_net_label, c, 0);
        lv_obj_remove_flag(s_net_label, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align_to(s_net_label, s_bar_time, LV_ALIGN_OUT_LEFT_MID, -14, -2);
    }

    radio_status_t rs;
    plat_radio_get_status(&rs);
    if (rs.state != RADIO_IDLE && rs.state != RADIO_ERROR) {
        lv_obj_t *anchor = st.active == NET_ACTIVE_ETH ? s_net_icon : s_net_label;
        lv_obj_update_layout(anchor);
        audio_out_t ao;
        plat_audio_out_get(&ao);
        lv_label_set_text(s_audio_lbl, ao.active ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(s_audio_lbl, rs.state == RADIO_PLAYING ? UI_C_ACCENT : UI_C_WARN, 0);
        lv_obj_remove_flag(s_audio_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align_to(s_audio_lbl, anchor, LV_ALIGN_OUT_LEFT_MID, -14, 0);
    } else {
        lv_obj_add_flag(s_audio_lbl, LV_OBJ_FLAG_HIDDEN);
    }

    /* bell: an alarm is armed (muted), snoozing (amber) or ringing (orange) */
    bool snoozed = alarm_snoozed(NULL);
    if (alarm_ringing() || snoozed || alarm_any_on()) {
        lv_obj_t *anchor = !lv_obj_has_flag(s_audio_lbl, LV_OBJ_FLAG_HIDDEN) ? s_audio_lbl
                         : st.active == NET_ACTIVE_ETH ? s_net_icon : s_net_label;
        lv_obj_update_layout(anchor);
        lv_label_set_text(s_alarm_lbl, snoozed ? LV_SYMBOL_BELL " z" : LV_SYMBOL_BELL);
        lv_obj_set_style_text_color(s_alarm_lbl, alarm_ringing() ? UI_C_ACCENT2 : snoozed ? UI_C_WARN : UI_C_MUTED, 0);
        lv_obj_remove_flag(s_alarm_lbl, LV_OBJ_FLAG_HIDDEN);
        lv_obj_align_to(s_alarm_lbl, anchor, LV_ALIGN_OUT_LEFT_MID, -14, anchor == s_audio_lbl ? 0 : 2);
    } else {
        lv_obj_add_flag(s_alarm_lbl, LV_OBJ_FLAG_HIDDEN);
    }
    if (s_saver_alarm) {
        char a[48];
        alarm_status_text(a, sizeof(a));
        if (a[0]) {
            lv_label_set_text_fmt(s_saver_alarm, LV_SYMBOL_BELL "  %s", a);
            lv_obj_remove_flag(s_saver_alarm, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_saver_alarm, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_saver_np) {
        if (rs.state == RADIO_PLAYING || rs.state == RADIO_BUFFERING) {
            char np[256];
            snprintf(np, sizeof(np), "%s%s%s", rs.name, rs.title[0] ? "  -  " : "", rs.title);
            lv_label_set_text(s_saver_np, np);
            lv_obj_remove_flag(s_saver_np, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_saver_np, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/* ------------------------------------------------------------------ launcher */

static void tile_cb(lv_event_t *e)
{
    app_id_t id = (app_id_t)(intptr_t)lv_event_get_user_data(e);
    ui_open_app(id);
}

static void update_home_clock(void)
{
    if (!s_home_time) return;
    time_t now = plat_now();
    struct tm t;
    localtime_r(&now, &t);
    char buf[48];
    ui_fmt_time(buf, sizeof(buf), &t, false, true);
    lv_label_set_text(s_home_time, buf);
    strftime(buf, sizeof(buf), "%A, %B %e", &t);
    /* collapse the space strftime pads %e with */
    char *dbl = strstr(buf, "  ");
    if (dbl) memmove(dbl, dbl + 1, strlen(dbl));
    lv_label_set_text(s_home_date, buf);
    if (s_home_wx) {
        const lv_image_dsc_t *ic;
        char w[80];
        if (wx_home_summary(&ic, w, sizeof(w))) {
            if (lv_image_get_src(s_home_wx_icon) != ic) lv_image_set_src(s_home_wx_icon, ic);
            lv_label_set_text(s_home_wx_lbl, w);
            lv_obj_remove_flag(s_home_wx, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_home_wx, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_home_alarm) {
        char a[48];
        alarm_status_text(a, sizeof(a));
        if (a[0]) {
            lv_label_set_text_fmt(s_home_alarm, LV_SYMBOL_BELL "  %s", a);
            lv_obj_set_style_text_color(s_home_alarm, alarm_snoozed(NULL) ? UI_C_WARN : UI_C_ACCENT2, 0);
            lv_obj_remove_flag(s_home_alarm, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_home_alarm, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

static void home_delete_cb(lv_event_t *e)
{
    (void)e;
    s_home_time = NULL;
    s_home_date = NULL;
    s_home_alarm = NULL;
    s_home_wx = s_home_wx_icon = s_home_wx_lbl = NULL;
}

static void build_launcher(void)
{
    lv_obj_t *c = s_content;
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_ver(c, 10, 0);
    lv_obj_set_style_pad_row(c, 0, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(c);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(hdr, home_delete_cb, LV_EVENT_DELETE, NULL);
    s_home_time = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_home_time, ui_font(UI_FW_LIGHT, 120), 0);
    lv_obj_set_style_text_color(s_home_time, UI_C_TEXT, 0);
    s_home_date = lv_label_create(hdr);
    lv_obj_set_style_text_font(s_home_date, ui_font(UI_FW_REGULAR, 30), 0);
    lv_obj_set_style_text_color(s_home_date, UI_C_MUTED, 0);
    lv_obj_set_style_margin_top(s_home_date, -26, 0);
    /* one line under the date: next alarm and current weather */
    lv_obj_t *info = lv_obj_create(hdr);
    lv_obj_remove_style_all(info);
    lv_obj_set_size(info, LV_SIZE_CONTENT, 44);
    lv_obj_set_flex_flow(info, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(info, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(info, 34, 0);
    lv_obj_set_style_margin_top(info, 2, 0);
    s_home_alarm = lv_label_create(info);
    lv_obj_set_style_text_font(s_home_alarm, &lv_font_montserrat_24, 0);
    lv_obj_add_flag(s_home_alarm, LV_OBJ_FLAG_HIDDEN);
    s_home_wx = lv_obj_create(info);
    lv_obj_remove_style_all(s_home_wx);
    lv_obj_set_size(s_home_wx, LV_SIZE_CONTENT, 44);
    lv_obj_set_flex_flow(s_home_wx, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(s_home_wx, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(s_home_wx, 6, 0);
    lv_obj_add_flag(s_home_wx, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_home_wx, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)APP_WEATHER);
    s_home_wx_icon = lv_image_create(s_home_wx);
    lv_image_set_src(s_home_wx_icon, &wx_03_s);
    lv_image_set_scale(s_home_wx_icon, 180);
    lv_obj_set_size(s_home_wx_icon, 44, 44);
    lv_image_set_inner_align(s_home_wx_icon, LV_IMAGE_ALIGN_CENTER);
    s_home_wx_lbl = lv_label_create(s_home_wx);
    lv_obj_set_style_text_font(s_home_wx_lbl, ui_font(UI_FW_REGULAR, 26), 0);
    lv_obj_set_style_text_color(s_home_wx_lbl, UI_C_TEXT, 0);
    lv_obj_add_flag(s_home_wx, LV_OBJ_FLAG_HIDDEN);
    update_home_clock();

    lv_obj_t *grid = lv_obj_create(c);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, 688, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(grid, 16, 0);
    lv_obj_set_style_pad_row(grid, 16, 0);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);

    for (int i = 0; i < APP_COUNT; i++) {
        lv_obj_t *t = lv_button_create(grid);
        lv_obj_set_size(t, 160, 164);
        lv_obj_set_style_bg_color(t, UI_C_SURFACE, 0);
        lv_obj_set_style_bg_color(t, UI_C_SURFACE2, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(t, 1, 0);
        lv_obj_set_style_border_color(t, UI_C_BORDER, 0);
        lv_obj_set_style_radius(t, 24, 0);
        lv_obj_set_style_shadow_width(t, 0, 0);
        lv_obj_set_style_pad_all(t, 8, 0);
        lv_obj_set_style_transform_scale(t, 245, LV_STATE_PRESSED);
        lv_obj_set_style_transform_pivot_x(t, 80, 0);
        lv_obj_set_style_transform_pivot_y(t, 82, 0);
        lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_flex_align(t, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_pad_row(t, 6, 0);
        lv_obj_add_event_cb(t, tile_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);

        lv_obj_t *img = lv_image_create(t);
        lv_image_set_src(img, s_apps[i]->icon);
        lv_image_set_scale(img, 220);   /* 112 px icon -> ~96 px */
        lv_obj_set_size(img, 96, 96);
        lv_image_set_inner_align(img, LV_IMAGE_ALIGN_CENTER);

        lv_obj_t *l = lv_label_create(t);
        lv_label_set_text(l, s_apps[i]->name);
        lv_obj_set_style_text_font(l, ui_font(UI_FW_MEDIUM, 20), 0);
        lv_obj_set_style_text_color(l, UI_C_TEXT, 0);
    }
}

/* ------------------------------------------------------------------ navigation */

static void reset_content(void)
{
    if (s_current >= 0 && s_apps[s_current]->destroy) s_apps[s_current]->destroy();
    lv_obj_clean(s_content);
    lv_obj_set_scroll_dir(s_content, LV_DIR_VER);
    lv_obj_set_layout(s_content, LV_LAYOUT_NONE);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_set_style_pad_row(s_content, 0, 0);
    lv_obj_set_style_pad_column(s_content, 0, 0);
    lv_obj_scroll_to(s_content, 0, 0, LV_ANIM_OFF);
}

void ui_go_home(void)
{
    reset_content();
    s_current = -1;
    lv_obj_add_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_title, "");
    build_launcher();
}

void ui_open_app(app_id_t id)
{
    if (id >= APP_COUNT) return;
    reset_content();
    s_current = id;
    lv_obj_remove_flag(s_back, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(s_title, s_apps[id]->name);
    s_apps[id]->create(s_content);
}

/* ------------------------------------------------------------------ screensaver */

static bool s_saver_closing;

static void saver_hide(void)
{
    if (!s_saver || s_saver_closing) return;
    s_saver_closing = true;
    lv_obj_delete_async(s_saver);   /* may be called from inside its own click event */
}

static void saver_deleted_cb(lv_event_t *e)
{
    (void)e;
    s_saver = NULL;
    s_saver_np = NULL;
    s_saver_alarm = NULL;
    s_saver_closing = false;
}

static void saver_click_cb(lv_event_t *e)
{
    (void)e;
    saver_hide();
}

static void saver_show(void)
{
    if (s_saver) return;
    s_saver = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_saver);
    lv_obj_set_size(s_saver, UI_W, UI_H);
    lv_obj_set_style_bg_color(s_saver, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_saver, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_saver, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_saver, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_saver, saver_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_event_cb(s_saver, saver_deleted_cb, LV_EVENT_DELETE, NULL);
    if (g_settings.saver_mode == SAVER_PHOTOS && photo_frame_create(s_saver)) {
        update_bar();
        return;   /* photo frame: no clock face, no now-playing line */
    }
    lv_obj_t *face = clock_face_create(s_saver, true);
    lv_obj_remove_flag(face, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(face, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_saver_np = lv_label_create(s_saver);
    lv_obj_set_width(s_saver_np, 640);
    lv_label_set_long_mode(s_saver_np, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_align(s_saver_np, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_saver_np, ui_font(UI_FW_REGULAR, 22), 0);
    lv_obj_set_style_text_color(s_saver_np, UI_C_ACCENT, 0);
    lv_obj_align(s_saver_np, LV_ALIGN_BOTTOM_MID, 0, -22);
    lv_obj_add_flag(s_saver_np, LV_OBJ_FLAG_HIDDEN);

    s_saver_alarm = lv_label_create(s_saver);
    lv_obj_set_style_text_font(s_saver_alarm, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_saver_alarm, UI_C_MUTED, 0);
    lv_obj_align(s_saver_alarm, LV_ALIGN_TOP_MID, 0, 26);
    lv_obj_add_flag(s_saver_alarm, LV_OBJ_FLAG_HIDDEN);
    update_bar();
}

void ui_wake(void)
{
    saver_hide();
    lv_display_trigger_activity(NULL);
    apply_backlight(true);
}

void ui_refresh_alarm_icons(void)
{
    update_bar();
    update_home_clock();
}

/* ------------------------------------------------------------------ ticks */

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    update_bar();
    update_home_clock();
    apply_backlight(false);
    lock_tick();
    alarm_tick();

    if (!s_saver && g_settings.screensaver_s > 0 &&
        lv_display_get_inactive_time(NULL) > (uint32_t)g_settings.screensaver_s * 1000u) {
        /* Don't cover a modal dialog the user is typing in. */
        bool modal_open = lv_obj_get_child_count(lv_layer_top()) > 0;
        if (!modal_open) saver_show();
    }
}

static void backlight_timer_cb(lv_timer_t *t)
{
    (void)t;
    apply_backlight(false);
}

/* ------------------------------------------------------------------ init */

void ui_init(void)
{
    lv_display_t *disp = lv_display_get_default();
    lv_theme_t *th = lv_theme_default_init(disp, UI_C_ACCENT, UI_C_ACCENT2, true, &lv_font_montserrat_20);
    lv_display_set_theme(disp, th);

    s_scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_scr, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(s_scr, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(s_scr, UI_C_TEXT, 0);
    lv_obj_remove_flag(s_scr, LV_OBJ_FLAG_SCROLLABLE);

    build_bar();

    s_content = lv_obj_create(s_scr);
    lv_obj_remove_style_all(s_content);
    lv_obj_set_size(s_content, UI_W, UI_H - UI_BAR_H);
    lv_obj_set_pos(s_content, 0, UI_BAR_H);
    lv_obj_set_style_bg_color(s_content, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_COVER, 0);
    lv_obj_set_scrollbar_mode(s_content, LV_SCROLLBAR_MODE_ACTIVE);

    lv_screen_load(s_scr);
    ui_go_home();
    update_bar();
    apply_backlight(true);

    lv_timer_create(tick_cb, 1000, NULL);
    lv_timer_create(backlight_timer_cb, 150, NULL);   /* wakes a night-dimmed screen right at the touch */

    s_save_timer = lv_timer_create(save_timer_cb, 1500, NULL);
    lv_timer_set_repeat_count(s_save_timer, -1);
    lv_timer_pause(s_save_timer);
}

/* Exposed for the simulator so it can capture the screensaver. */
void ui_debug_show_screensaver(void) { saver_show(); }
