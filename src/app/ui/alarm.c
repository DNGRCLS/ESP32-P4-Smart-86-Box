/*
 * Alarms: scheduling, snooze, the ringing screen and the alarm sound.
 *
 * Everything runs in the LVGL task: alarm_tick() is called once a second by ui_core.
 * Sounds:  wake-to-radio (the last station played)  |  beeps  |  buzzer.
 * Wake-to-radio falls back to beeps if the stream has not started after 25 s, or drops out
 * for 20 s, so an alarm always makes a sound.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_internal.h"
#include "../core/stations.h"

#define RADIO_START_GRACE_S 25
#define RADIO_DROP_GRACE_S  20
#define FADE_S              30

static int s_ring = -1;            /* slot ringing, ALARM_TEST for a test ring, -1 none */
static alarm_t s_ring_alarm;       /* copy of what is ringing (a "once" alarm is disarmed when it rings) */
static time_t s_ring_start;
static int s_snooze_slot = -1;     /* slot being snoozed, -1 none */
static alarm_t s_snooze_alarm;
static time_t s_snooze_until;
static int s_last_key = -1;        /* local minute last checked */

/* sound currently playing */
static int s_snd;                  /* alarm_sound_t actually in use */
static bool s_radio_started;       /* we started (or took over) a stream */
static time_t s_radio_bad_since;   /* stream not playing since; 0 = fine */
static bool s_fell_back;
static int s_vol_applied = -1;

/* ringing screen */
static lv_obj_t *s_ov, *s_ov_time, *s_ov_info, *s_ov_snooze_lbl;

#define ALARM_TEST 99

/* ------------------------------------------------------------------ helpers */

bool alarm_valid_time(void)
{
    time_t now = plat_now();
    struct tm t;
    localtime_r(&now, &t);
    return t.tm_year >= 124;   /* 2024 or later: the clock has been set */
}

void alarm_fmt_time(char *buf, size_t n, int hour, int minute)
{
    struct tm t = {0};
    t.tm_hour = hour;
    t.tm_min = minute;
    ui_fmt_time(buf, n, &t, false, true);
}

void alarm_fmt_days(const alarm_t *a, char *buf, size_t n)
{
    static const char *names[7] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};
    uint8_t d = a->days & 0x7F;
    if (d == 0) { snprintf(buf, n, "Once"); return; }
    if (d == 0x7F) { snprintf(buf, n, "Every day"); return; }
    if (d == 0x3E) { snprintf(buf, n, "Weekdays"); return; }
    if (d == 0x41) { snprintf(buf, n, "Weekends"); return; }
    buf[0] = 0;
    size_t len = 0;
    for (int i = 1; i <= 7; i++) {          /* Monday first */
        int k = i % 7;
        if (d & (1 << k)) len += snprintf(buf + len, len < n ? n - len : 0, "%s%s", len ? " " : "", names[k]);
    }
}

const char *alarm_sound_name(int snd)
{
    switch (snd) {
    case ALARM_SND_RADIO: return "Radio";
    case ALARM_SND_BUZZ: return "Buzzer";
    default: return "Beeps";
    }
}

/* Station used for wake-to-radio: the last stream played, else the last saved station, else the first. */
bool alarm_station(const char **name, const char **url)
{
    if (g_settings.last_play_url[0]) {
        *name = g_settings.last_play_name[0] ? g_settings.last_play_name : "Radio";
        *url = g_settings.last_play_url;
        return true;
    }
    int i = g_settings.radio_last;
    if (i < 0 || i >= g_stations.count) i = 0;
    if (i < g_stations.count) {
        *name = g_stations.items[i].name;
        *url = g_stations.items[i].url;
        return true;
    }
    return false;
}

/* When an alarm next rings after `now`: today/tomorrow for a one-time alarm, the next selected
 * weekday otherwise.  mktime() handles daylight-saving changes. */
static time_t next_ring(const alarm_t *a, time_t now)
{
    struct tm base;
    localtime_r(&now, &base);
    for (int d = 0; d <= 7; d++) {
        struct tm t = base;
        t.tm_mday += d;
        t.tm_hour = a->hour;
        t.tm_min = a->minute;
        t.tm_sec = 0;
        t.tm_isdst = -1;
        time_t when = mktime(&t);
        if (when <= now) continue;
        if (a->days && !(a->days & (1 << t.tm_wday))) continue;
        return when;
    }
    return 0;
}

bool alarm_next(time_t *when, int *slot)
{
    time_t now = plat_now(), best = 0;
    int bi = -1;
    for (int i = 0; i < ALARM_MAX; i++) {
        const alarm_t *a = &g_settings.alarms[i];
        if (!a->used || !a->on) continue;
        time_t w = next_ring(a, now);
        if (w && (!best || w < best)) { best = w; bi = i; }
    }
    if (when) *when = best;
    if (slot) *slot = bi;
    return bi >= 0;
}

bool alarm_any_on(void)
{
    for (int i = 0; i < ALARM_MAX; i++)
        if (g_settings.alarms[i].used && g_settings.alarms[i].on) return true;
    return false;
}

/* "Tue 6:30 AM", or "6:30 AM" when it is within the next 24 hours */
void alarm_fmt_when(char *buf, size_t n, time_t when)
{
    struct tm t;
    localtime_r(&when, &t);
    char tm[24];
    ui_fmt_time(tm, sizeof(tm), &t, false, true);
    if (when - plat_now() < 20 * 3600) {
        snprintf(buf, n, "%s", tm);
    } else {
        char day[8];
        strftime(day, sizeof(day), "%a", &t);
        snprintf(buf, n, "%s %s", day, tm);
    }
}

bool alarm_ringing(void) { return s_ring >= 0; }

bool alarm_snoozed(time_t *until)
{
    if (s_snooze_slot < 0) return false;
    if (until) *until = s_snooze_until;
    return true;
}

/* Text for the clock screen / home screen, "" when nothing is set. */
void alarm_status_text(char *buf, size_t n)
{
    time_t when;
    char w[32];
    if (s_snooze_slot >= 0) {
        alarm_fmt_when(w, sizeof(w), s_snooze_until);
        snprintf(buf, n, "Snoozed until %s", w);
    } else if (alarm_next(&when, NULL)) {
        alarm_fmt_when(w, sizeof(w), when);
        snprintf(buf, n, "%s", w);
    } else {
        buf[0] = 0;
    }
}

/* ------------------------------------------------------------------ sound */

static void set_volume(int v)
{
    if (v < 5) v = 5;
    if (v != s_vol_applied) {
        s_vol_applied = v;
        plat_alarm_volume(v);
    }
}

static void update_volume(void)
{
    int target = g_settings.alarm_volume;
    if (g_settings.alarm_fade) {
        long el = (long)(plat_now() - s_ring_start);
        if (el < FADE_S) target = target * (20 + 80 * el / FADE_S) / 100;
    }
    set_volume(target);
}

static void tone_start(int snd)
{
    s_snd = snd;
    plat_alarm_tone(snd == ALARM_SND_BUZZ ? 2 : 1);
}

static void sound_start(const alarm_t *a)
{
    s_fell_back = false;
    s_radio_started = false;
    s_radio_bad_since = 0;
    s_vol_applied = -1;
    radio_status_t rs;
    plat_radio_get_status(&rs);
    bool radio_on = rs.state != RADIO_IDLE && rs.state != RADIO_ERROR;
    update_volume();
    if (a->sound == ALARM_SND_RADIO) {
        const char *name, *url;
        s_snd = ALARM_SND_RADIO;
        if (radio_on) {
            s_radio_started = true;      /* already playing: just turn it up, on the built-in speaker */
            plat_alarm_speaker(true);
        } else if (alarm_station(&name, &url)) {
            plat_alarm_radio(name, url);
            s_radio_started = true;
        } else {
            s_fell_back = true;          /* no station at all */
            tone_start(ALARM_SND_BEEP);
        }
    } else {
        if (radio_on) plat_alarm_radio_stop();
        tone_start(a->sound);
    }
}

static void sound_stop(bool keep_radio)
{
    plat_alarm_tone(0);
    if (s_radio_started && !keep_radio) plat_alarm_radio_stop();
    s_radio_started = false;
    plat_alarm_volume(g_settings.radio_volume);
    s_vol_applied = -1;
}

static void overlay_show(void);

/* Wake-to-radio watchdog: switch to beeps if the stream never starts or stays down. */
static void radio_watch(void)
{
    if (s_snd != ALARM_SND_RADIO || !s_radio_started) return;
    radio_status_t rs;
    plat_radio_get_status(&rs);
    time_t now = plat_now();
    bool ok = rs.state == RADIO_PLAYING || rs.state == RADIO_BUFFERING;
    bool dead = rs.state == RADIO_ERROR || rs.state == RADIO_IDLE;
    if (ok && rs.state == RADIO_PLAYING) { s_radio_bad_since = 0; return; }
    if (!s_radio_bad_since) s_radio_bad_since = now;
    long grace = (now - s_ring_start) < RADIO_START_GRACE_S ? RADIO_START_GRACE_S : RADIO_DROP_GRACE_S;
    if (dead || now - s_radio_bad_since >= grace) {
        plat_alarm_radio_stop();
        s_radio_started = false;
        s_fell_back = true;
        tone_start(ALARM_SND_BEEP);
        overlay_show();   /* rebuilt without the "Keep listening" button */
    }
}

/* ------------------------------------------------------------------ ringing screen */

static void stop_ringing(bool keep_radio);

static void stop_cb(lv_event_t *e) { (void)e; alarm_stop(); }
static void snooze_cb(lv_event_t *e) { (void)e; alarm_snooze(); }

static void keep_cb(lv_event_t *e)
{
    (void)e;
    stop_ringing(true);
    /* the stream becomes ordinary listening: it resumes after a power cut like any other */
    g_settings.radio_was_playing = true;
    ui_settings_changed();
}

static void ov_deleted(lv_event_t *e)
{
    (void)e;
    s_ov = s_ov_time = s_ov_info = s_ov_snooze_lbl = NULL;
}

static void bell_anim_cb(void *obj, int32_t v) { lv_obj_set_style_transform_rotation((lv_obj_t *)obj, v, 0); }

static void overlay_info(void)
{
    if (!s_ov_info) return;
    char buf[128];
    if (s_snd == ALARM_SND_RADIO) {
        radio_status_t rs;
        plat_radio_get_status(&rs);
        const char *st = rs.state == RADIO_PLAYING ? "" : "  (connecting...)";
        snprintf(buf, sizeof(buf), LV_SYMBOL_AUDIO "  %s%s", rs.name[0] ? rs.name : "Radio", st);
    } else if (s_fell_back) {
        snprintf(buf, sizeof(buf), LV_SYMBOL_WARNING "  Radio unavailable - playing beeps");
    } else {
        snprintf(buf, sizeof(buf), LV_SYMBOL_BELL "  %s", alarm_sound_name(s_snd));
    }
    lv_label_set_text(s_ov_info, buf);
}

static void overlay_update(void)
{
    if (!s_ov) return;
    time_t now = plat_now();
    struct tm t;
    localtime_r(&now, &t);
    char buf[32];
    ui_fmt_time(buf, sizeof(buf), &t, false, true);
    lv_label_set_text(s_ov_time, buf);
    overlay_info();
}

static lv_obj_t *ov_button(lv_obj_t *parent, const char *text, lv_color_t bg, lv_color_t fg, int w, int h,
                           const lv_font_t *font, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_radius(b, 28, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, fg, 0);
    lv_obj_center(l);
    return b;
}

static void overlay_show(void)
{
    if (s_ov) lv_obj_delete(s_ov);
    lv_obj_t *o = lv_obj_create(lv_layer_top());
    s_ov = o;
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, UI_W, UI_H);
    lv_obj_set_style_bg_color(o, lv_color_hex(0x05070A), 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);   /* swallow touches meant for what is underneath */
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_all(o, 30, 0);
    lv_obj_set_style_pad_row(o, 8, 0);
    lv_obj_add_event_cb(o, ov_deleted, LV_EVENT_DELETE, NULL);

    lv_obj_t *bell = lv_label_create(o);
    lv_label_set_text(bell, LV_SYMBOL_BELL);
    lv_obj_set_style_text_font(bell, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(bell, UI_C_ACCENT2, 0);
    lv_obj_update_layout(bell);
    lv_obj_set_style_transform_pivot_x(bell, lv_obj_get_width(bell) / 2, 0);
    lv_obj_set_style_transform_pivot_y(bell, 4, 0);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, bell);
    lv_anim_set_exec_cb(&a, bell_anim_cb);
    lv_anim_set_values(&a, -180, 180);
    lv_anim_set_duration(&a, 140);
    lv_anim_set_reverse_duration(&a, 140);
    lv_anim_set_repeat_delay(&a, 500);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);

    s_ov_time = lv_label_create(o);
    lv_obj_set_style_text_font(s_ov_time, ui_font(UI_FW_LIGHT, 150), 0);
    lv_obj_set_style_text_color(s_ov_time, UI_C_TEXT, 0);
    lv_obj_set_style_margin_top(s_ov_time, -10, 0);

    lv_obj_t *lbl = lv_label_create(o);
    lv_label_set_text(lbl, s_ring_alarm.label[0] ? s_ring_alarm.label : (s_ring == ALARM_TEST ? "Test alarm" : "Alarm"));
    lv_obj_set_style_text_font(lbl, ui_font(UI_FW_MEDIUM, 34), 0);
    lv_obj_set_style_text_color(lbl, UI_C_ACCENT2, 0);
    lv_obj_set_style_margin_top(lbl, -24, 0);

    s_ov_info = lv_label_create(o);
    lv_obj_set_width(s_ov_info, 640);
    lv_label_set_long_mode(s_ov_info, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_ov_info, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_ov_info, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_ov_info, UI_C_MUTED, 0);
    lv_obj_set_style_margin_bottom(s_ov_info, 18, 0);

    char sn[32];
    snprintf(sn, sizeof(sn), "Snooze  %d min", g_settings.alarm_snooze_min);
    lv_obj_t *sb = ov_button(o, sn, UI_C_ACCENT, lv_color_white(), 640, 190, ui_font(UI_FW_MEDIUM, 46), snooze_cb);
    s_ov_snooze_lbl = lv_obj_get_child(sb, 0);

    lv_obj_t *row = lv_obj_create(o);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, 640, 110);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_margin_top(row, 8, 0);
    bool radio = s_snd == ALARM_SND_RADIO;
    ov_button(row, "Stop", UI_C_SURFACE2, UI_C_ERR, radio ? 312 : 640, 110, ui_font(UI_FW_MEDIUM, 40), stop_cb);
    if (radio) ov_button(row, "Keep listening", UI_C_SURFACE2, UI_C_TEXT, 312, 110, ui_font(UI_FW_MEDIUM, 30), keep_cb);

    overlay_update();
}

/* ------------------------------------------------------------------ ring / stop / snooze */

static void ring(int slot, const alarm_t *a)
{
    if (s_ring >= 0) return;            /* already ringing: let the current one go on */
    s_ring = slot;
    s_ring_alarm = *a;
    s_ring_start = plat_now();
    s_snooze_slot = -1;
    if (slot >= 0 && slot < ALARM_MAX && !a->days) {
        g_settings.alarms[slot].on = false;   /* one-time alarm: disarm (snooze still works) */
        ui_settings_changed();
    }
    ui_wake();
    sound_start(&s_ring_alarm);
    overlay_show();
}

static void stop_ringing(bool keep_radio)
{
    if (s_ring < 0) return;
    sound_stop(keep_radio);
    s_ring = -1;
    if (s_ov) lv_obj_delete_async(s_ov);
    ui_refresh_alarm_icons();
}

void alarm_stop(void)
{
    stop_ringing(false);
    s_snooze_slot = -1;
    ui_refresh_alarm_icons();
}

void alarm_snooze(void)
{
    if (s_ring < 0) return;
    int slot = s_ring;
    alarm_t a = s_ring_alarm;
    stop_ringing(false);
    s_snooze_slot = slot;
    s_snooze_alarm = a;
    s_snooze_until = plat_now() + g_settings.alarm_snooze_min * 60;
    char w[32], msg[64];
    alarm_fmt_when(w, sizeof(w), s_snooze_until);
    snprintf(msg, sizeof(msg), "Snoozing until %s", w);
    ui_toast(msg);
    ui_refresh_alarm_icons();
}

int alarm_snooze_slot(void) { return s_snooze_slot; }

void alarm_snooze_cancel(void)
{
    s_snooze_slot = -1;
    ui_refresh_alarm_icons();
}

void alarm_test(const alarm_t *a)
{
    if (s_ring >= 0) return;
    alarm_t t = *a;
    t.days = 0x7F;   /* never disarm anything */
    ring(ALARM_TEST, &t);
}

/* ------------------------------------------------------------------ tick */

void alarm_tick(void)
{
    if (!alarm_valid_time()) return;
    time_t now = plat_now();
    struct tm t;
    localtime_r(&now, &t);
    int key = (t.tm_yday * 24 + t.tm_hour) * 60 + t.tm_min;

    if (s_last_key < 0) s_last_key = key - 1;   /* first valid time: still ring if we booted inside the alarm minute */
    if (key != s_last_key) {
        s_last_key = key;
        for (int i = 0; i < ALARM_MAX; i++) {
            alarm_t *a = &g_settings.alarms[i];
            if (!a->used || !a->on || a->hour != t.tm_hour || a->minute != t.tm_min) continue;
            if (a->days && !(a->days & (1 << t.tm_wday))) continue;
            if (s_ring >= 0) break;
            ring(i, a);
            break;
        }
    }

    if (s_snooze_slot >= 0 && s_ring < 0 && now >= s_snooze_until) {
        int slot = s_snooze_slot;
        alarm_t a = s_snooze_alarm;
        s_snooze_slot = -1;
        ring(slot, &a);
    }

    if (s_ring >= 0) {
        update_volume();
        radio_watch();
        overlay_update();
        if (now - s_ring_start >= (time_t)g_settings.alarm_ring_min * 60) {
            stop_ringing(false);
            s_snooze_slot = -1;
            ui_toast("Alarm stopped by itself");
        }
    }
}

/* Settings changed elsewhere (web page): a deleted or disarmed slot can't stay snoozed. */
void alarm_settings_changed(void)
{
    if (s_snooze_slot >= 0 && s_snooze_slot < ALARM_MAX && !g_settings.alarms[s_snooze_slot].used) s_snooze_slot = -1;
    if (s_ov_snooze_lbl) lv_label_set_text_fmt(s_ov_snooze_lbl, "Snooze  %d min", g_settings.alarm_snooze_min);
}


/* ------------------------------------------------------------------ web page */

void ui_alarm_status(bool *ringing, char *label, size_t ln, char *next, size_t nn)
{
    *ringing = s_ring >= 0;
    snprintf(label, ln, "%s", s_ring < 0 ? "" : s_ring_alarm.label[0] ? s_ring_alarm.label : "Alarm");
    alarm_status_text(next, nn);
}

void ui_alarm_stop(void) { alarm_stop(); }
void ui_alarm_snooze(void) { alarm_snooze(); }
