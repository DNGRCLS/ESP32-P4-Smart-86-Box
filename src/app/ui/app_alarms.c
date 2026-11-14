/*
 * Alarms page of the Clock app: list of alarms, options (snooze, volume, fade, auto-stop)
 * and the full-screen alarm editor.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_internal.h"

static lv_obj_t *s_page;
static void (*s_on_done)(void);
static lv_obj_t *s_snooze_val, *s_vol_val;

/* ------------------------------------------------------------------ small helpers */

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    return l;
}

static lv_obj_t *pill(lv_obj_t *parent, const char *text, int w, int h, bool checked, lv_event_cb_t cb, void *user)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_style_radius(b, h / 2, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, UI_C_SURFACE2, 0);
    lv_obj_set_style_bg_color(b, UI_C_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, UI_C_MUTED, 0);
    lv_obj_set_style_text_color(b, lv_color_white(), LV_STATE_CHECKED);
    lv_obj_set_state(b, LV_STATE_CHECKED, checked);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, user);
    return b;
}

static lv_obj_t *hrow(lv_obj_t *parent, int gap)
{
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

static void fmt_eta(char *buf, size_t n, time_t when)
{
    long s = (long)(when - plat_now());
    if (s < 60) s = 60;
    long h = s / 3600, m = (s % 3600 + 59) / 60;
    if (m == 60) { h++; m = 0; }
    if (h >= 24) snprintf(buf, n, "%ld d %ld h", h / 24, h % 24);
    else if (h) snprintf(buf, n, "%ld h %ld min", h, m);
    else snprintf(buf, n, "%ld min", m);
}

static void toast_eta(int slot)
{
    time_t when;
    int next;
    /* rings-in time of this alarm specifically */
    alarm_t saved[ALARM_MAX];
    memcpy(saved, g_settings.alarms, sizeof(saved));
    for (int i = 0; i < ALARM_MAX; i++) if (i != slot) g_settings.alarms[i].on = false;
    bool ok = alarm_next(&when, &next);
    memcpy(g_settings.alarms, saved, sizeof(saved));
    if (!ok) return;
    char eta[32], msg[80];
    fmt_eta(eta, sizeof(eta), when);
    snprintf(msg, sizeof(msg), "Alarm rings in %s", eta);
    ui_toast(msg);
}

/* ================================================================== editor */

typedef struct {
    lv_obj_t *root;
    int slot;                 /* -1 = new */
    alarm_t a;
    lv_obj_t *r_hour, *r_min, *r_ampm;
    lv_obj_t *day_btn[7];
    lv_obj_t *snd_btn[3];
    lv_obj_t *snd_note;
    lv_obj_t *label_val;
} editor_t;

static editor_t *s_ed;

static const char *s_min_opts;
static char s_min_buf[60 * 3 + 1];
static char s_h12_buf[12 * 3 + 1];
static char s_h24_buf[24 * 3 + 1];

static void build_roller_opts(void)
{
    if (s_min_opts) return;
    char *p = s_min_buf;
    for (int i = 0; i < 60; i++) p += sprintf(p, "%02d%s", i, i < 59 ? "\n" : "");
    p = s_h12_buf;
    for (int i = 1; i <= 12; i++) p += sprintf(p, "%d%s", i, i < 12 ? "\n" : "");
    p = s_h24_buf;
    for (int i = 0; i < 24; i++) p += sprintf(p, "%02d%s", i, i < 23 ? "\n" : "");
    s_min_opts = s_min_buf;
}

static lv_obj_t *roller(lv_obj_t *parent, const char *opts, int sel, int w, bool infinite)
{
    lv_obj_t *r = lv_roller_create(parent);
    lv_roller_set_options(r, opts, infinite ? LV_ROLLER_MODE_INFINITE : LV_ROLLER_MODE_NORMAL);
    lv_obj_set_width(r, w);
    lv_obj_set_style_text_font(r, ui_font(UI_FW_MEDIUM, 42), 0);
    lv_obj_set_style_text_font(r, ui_font(UI_FW_MEDIUM, 42), LV_PART_SELECTED);
    lv_obj_set_style_text_color(r, UI_C_MUTED, 0);
    lv_obj_set_style_text_color(r, lv_color_white(), LV_PART_SELECTED);
    lv_obj_set_style_bg_color(r, UI_C_SURFACE, 0);
    lv_obj_set_style_bg_color(r, UI_C_ACCENT, LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(r, LV_OPA_COVER, LV_PART_SELECTED);
    lv_obj_set_style_radius(r, 16, 0);
    lv_obj_set_style_border_width(r, 1, 0);
    lv_obj_set_style_border_color(r, UI_C_BORDER, 0);
    lv_obj_set_style_text_align(r, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(r, 8, 0);
    lv_roller_set_visible_row_count(r, 3);   /* after the font: the height is computed from it */
    lv_roller_set_selected(r, sel, LV_ANIM_OFF);
    return r;
}

static void ed_read_time(editor_t *d)
{
    int m = lv_roller_get_selected(d->r_min);
    int h = lv_roller_get_selected(d->r_hour);
    if (d->r_ampm) {
        h = (h + 1) % 12;                                  /* "12" -> 0 */
        if (lv_roller_get_selected(d->r_ampm) == 1) h += 12;
    }
    d->a.hour = (uint8_t)h;
    d->a.minute = (uint8_t)m;
}

static void ed_sync(editor_t *d)
{
    for (int i = 0; i < 7; i++) lv_obj_set_state(d->day_btn[i], LV_STATE_CHECKED, (d->a.days >> i) & 1);
    for (int i = 0; i < 3; i++) lv_obj_set_state(d->snd_btn[i], LV_STATE_CHECKED, d->a.sound == i);
    const char *name, *url;
    char buf[128];
    if (d->a.sound == ALARM_SND_RADIO) {
        if (alarm_station(&name, &url)) snprintf(buf, sizeof(buf), "Plays the last station: %s", name);
        else snprintf(buf, sizeof(buf), "No station played yet: beeps will be used");
    } else {
        char days[40];
        alarm_fmt_days(&d->a, days, sizeof(days));
        snprintf(buf, sizeof(buf), "Repeat: %s", days);
    }
    lv_label_set_text(d->snd_note, buf);
    lv_label_set_text(d->label_val, d->a.label[0] ? d->a.label : "None");
}

static void ed_close(void)
{
    if (!s_ed || !s_ed->root) return;
    lv_obj_delete_async(s_ed->root);   /* s_ed is freed in the DELETE handler */
    s_ed->root = NULL;
}

static void ed_deleted(lv_event_t *e)
{
    editor_t *d = lv_event_get_user_data(e);
    if (s_ed == d) s_ed = NULL;
    free(d);
    alarms_page_refresh();
}

static void ed_day_cb(lv_event_t *e)
{
    if (!s_ed) return;
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_ed->a.days ^= (uint8_t)(1 << i);
    ed_sync(s_ed);
}

static void ed_preset_cb(lv_event_t *e)
{
    if (!s_ed) return;
    static const uint8_t presets[4] = {0x00, 0x3E, 0x41, 0x7F};
    s_ed->a.days = presets[(int)(intptr_t)lv_event_get_user_data(e)];
    ed_sync(s_ed);
}

static void ed_sound_cb(lv_event_t *e)
{
    if (!s_ed) return;
    s_ed->a.sound = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    ed_sync(s_ed);
}

static void ed_label_done(const char *text, void *user)
{
    (void)user;
    if (!s_ed) return;
    snprintf(s_ed->a.label, sizeof(s_ed->a.label), "%s", text);
    ed_sync(s_ed);
}

static void ed_label_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ed) return;
    ui_text_prompt("Alarm label", s_ed->a.label, false, sizeof(s_ed->a.label) - 1, ed_label_done, NULL);
}

static void ed_cancel_cb(lv_event_t *e)
{
    (void)e;
    ed_close();
}

static void ed_save_cb(lv_event_t *e)
{
    (void)e;
    editor_t *d = s_ed;
    if (!d) return;
    ed_read_time(d);
    int slot = d->slot;
    if (slot < 0) {
        for (int i = 0; i < ALARM_MAX; i++) if (!g_settings.alarms[i].used) { slot = i; break; }
        if (slot < 0) { ui_toast("All alarm slots are in use"); return; }
    }
    if (alarm_snooze_slot() == slot) alarm_snooze_cancel();   /* the time may have changed: start fresh */
    d->a.used = true;
    d->a.on = true;
    g_settings.alarms[slot] = d->a;
    ui_settings_changed();
    ed_close();
    if (alarm_valid_time()) toast_eta(slot);
    else ui_toast("Alarm saved - it rings once the clock is set from the network");
}

static void ed_delete_ok(void *user)
{
    (void)user;
    if (!s_ed || s_ed->slot < 0) return;
    if (alarm_snooze_slot() == s_ed->slot) alarm_snooze_cancel();
    memset(&g_settings.alarms[s_ed->slot], 0, sizeof(alarm_t));
    ui_settings_changed();
    ed_close();
}

static void ed_delete_cb(lv_event_t *e)
{
    (void)e;
    ui_confirm("Delete alarm?", "This alarm will be removed.", "Delete", UI_C_ERR, ed_delete_ok, NULL);
}

static void ed_test_cb(lv_event_t *e)
{
    (void)e;
    if (!s_ed) return;
    ed_read_time(s_ed);
    alarm_test(&s_ed->a);
}

static void editor_open(int slot)
{
    if (s_ed) return;
    build_roller_opts();
    editor_t *d = calloc(1, sizeof(editor_t));
    s_ed = d;
    d->slot = slot;
    if (slot >= 0) {
        d->a = g_settings.alarms[slot];
    } else {
        memset(&d->a, 0, sizeof(d->a));
        d->a.hour = 7;
        d->a.minute = 0;
        d->a.days = 0x3E;
        d->a.sound = ALARM_SND_RADIO;
    }

    lv_obj_t *o = lv_obj_create(lv_layer_top());
    d->root = o;
    lv_obj_remove_style_all(o);
    lv_obj_set_size(o, UI_W, UI_H);
    lv_obj_set_style_bg_color(o, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(o, 18, 0);
    lv_obj_set_style_pad_row(o, 10, 0);
    lv_obj_set_flex_flow(o, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(o, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_scroll_dir(o, LV_DIR_VER);
    lv_obj_add_event_cb(o, ed_deleted, LV_EVENT_DELETE, d);

    /* header: Cancel  |  title  |  Save */
    lv_obj_t *hdr = lv_obj_create(o);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), 60);
    lv_obj_t *cb = ui_button(hdr, "Cancel", UI_C_SURFACE2);
    lv_obj_align(cb, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_add_event_cb(cb, ed_cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *t = label(hdr, slot < 0 ? "New alarm" : "Edit alarm", ui_font(UI_FW_MEDIUM, 30), UI_C_TEXT);
    lv_obj_align(t, LV_ALIGN_CENTER, 0, 2);
    lv_obj_t *sb = ui_button(hdr, "Save", UI_C_ACCENT);
    lv_obj_set_width(sb, 120);
    lv_obj_align(sb, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(sb, ed_save_cb, LV_EVENT_CLICKED, NULL);

    /* time */
    lv_obj_t *tr = hrow(o, 14);
    lv_obj_set_flex_align(tr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    if (g_settings.clock_24h) {
        d->r_hour = roller(tr, s_h24_buf, d->a.hour, 150, true);
    } else {
        int h12 = d->a.hour % 12;
        d->r_hour = roller(tr, s_h12_buf, h12 == 0 ? 11 : h12 - 1, 150, true);
    }
    label(tr, ":", ui_font(UI_FW_MEDIUM, 56), UI_C_TEXT);
    d->r_min = roller(tr, s_min_opts, d->a.minute, 150, true);
    if (!g_settings.clock_24h) {
        lv_obj_t *gap = lv_obj_create(tr);
        lv_obj_remove_style_all(gap);
        lv_obj_set_size(gap, 10, 1);
        d->r_ampm = roller(tr, "AM\nPM", d->a.hour >= 12 ? 1 : 0, 140, false);
    }

    /* repeat */
    ui_section_title(o, "REPEAT");
    lv_obj_t *dr = hrow(o, 12);
    lv_obj_set_flex_align(dr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *dl[7] = {"S", "M", "T", "W", "T", "F", "S"};
    for (int i = 0; i < 7; i++) d->day_btn[i] = pill(dr, dl[i], 76, 76, false, ed_day_cb, (void *)(intptr_t)i);
    lv_obj_t *pr = hrow(o, 12);
    lv_obj_set_flex_align(pr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    static const char *pl[4] = {"Once", "Weekdays", "Weekends", "Every day"};
    for (int i = 0; i < 4; i++) {
        lv_obj_t *b = pill(pr, pl[i], 150, 46, false, ed_preset_cb, (void *)(intptr_t)i);
        lv_obj_set_style_bg_color(b, UI_C_SURFACE, 0);
        lv_obj_set_style_border_width(b, 1, 0);
        lv_obj_set_style_border_color(b, UI_C_BORDER, 0);
        lv_obj_set_style_text_font(lv_obj_get_child(b, 0), &lv_font_montserrat_16, 0);
    }

    /* sound */
    ui_section_title(o, "SOUND");
    lv_obj_t *sr = hrow(o, 12);
    lv_obj_set_flex_align(sr, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    d->snd_btn[ALARM_SND_RADIO] = pill(sr, LV_SYMBOL_AUDIO "  Radio", 212, 60, false, ed_sound_cb, (void *)(intptr_t)ALARM_SND_RADIO);
    d->snd_btn[ALARM_SND_BEEP] = pill(sr, LV_SYMBOL_BELL "  Beeps", 212, 60, false, ed_sound_cb, (void *)(intptr_t)ALARM_SND_BEEP);
    d->snd_btn[ALARM_SND_BUZZ] = pill(sr, LV_SYMBOL_BELL "  Buzzer", 212, 60, false, ed_sound_cb, (void *)(intptr_t)ALARM_SND_BUZZ);
    d->snd_note = label(o, "", &lv_font_montserrat_20, UI_C_MUTED);
    lv_obj_set_width(d->snd_note, lv_pct(100));
    lv_label_set_long_mode(d->snd_note, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(d->snd_note, LV_TEXT_ALIGN_CENTER, 0);

    /* label + delete / test */
    lv_obj_t *lr = hrow(o, 12);
    lv_obj_set_style_margin_top(lr, 6, 0);
    lv_obj_t *lb = lv_button_create(lr);
    lv_obj_set_height(lb, 60);
    lv_obj_set_flex_grow(lb, 1);
    lv_obj_set_style_bg_color(lb, UI_C_SURFACE, 0);
    lv_obj_set_style_border_width(lb, 1, 0);
    lv_obj_set_style_border_color(lb, UI_C_BORDER, 0);
    lv_obj_set_style_radius(lb, 14, 0);
    lv_obj_set_style_shadow_width(lb, 0, 0);
    lv_obj_add_event_cb(lb, ed_label_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ll = label(lb, LV_SYMBOL_EDIT "  Label", &lv_font_montserrat_20, UI_C_MUTED);
    lv_obj_align(ll, LV_ALIGN_LEFT_MID, 0, 0);
    d->label_val = label(lb, "", &lv_font_montserrat_20, UI_C_TEXT);
    lv_obj_set_width(d->label_val, 200);
    lv_label_set_long_mode(d->label_val, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(d->label_val, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(d->label_val, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_t *tb = ui_button(lr, LV_SYMBOL_PLAY "  Test", UI_C_SURFACE2);
    lv_obj_add_event_cb(tb, ed_test_cb, LV_EVENT_CLICKED, NULL);
    if (slot >= 0) {
        lv_obj_t *db = ui_button(lr, LV_SYMBOL_TRASH, UI_C_SURFACE2);
        lv_obj_set_style_text_color(db, UI_C_ERR, 0);
        lv_obj_add_event_cb(db, ed_delete_cb, LV_EVENT_CLICKED, NULL);
    }

    ed_sync(d);
}

/* ================================================================== page */

static void done_cb(lv_event_t *e)
{
    (void)e;
    if (s_on_done) s_on_done();
}

static void new_cb(lv_event_t *e)
{
    (void)e;
    editor_open(-1);
}

static void edit_cb(lv_event_t *e)
{
    editor_open((int)(intptr_t)lv_event_get_user_data(e));
}

static void switch_cb(lv_event_t *e)
{
    int slot = (int)(intptr_t)lv_event_get_user_data(e);
    bool on = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    g_settings.alarms[slot].on = on;
    if (!on && alarm_snooze_slot() == slot) alarm_snooze_cancel();
    ui_settings_changed();
    if (on && alarm_valid_time()) toast_eta(slot);
    alarms_page_refresh();
}

static void cancel_snooze_cb(lv_event_t *e)
{
    (void)e;
    alarm_snooze_cancel();
    alarms_page_refresh();
}

static void snooze_len_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    lv_label_set_text_fmt(s_snooze_val, "%d min", v);
    if (g_settings.alarm_snooze_min != v) {
        g_settings.alarm_snooze_min = (uint8_t)v;
        ui_settings_changed();
    }
}

static void vol_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    lv_label_set_text_fmt(s_vol_val, "%d %%", v);
    if (g_settings.alarm_volume != v) {
        g_settings.alarm_volume = (uint8_t)v;
        ui_settings_changed();
    }
}

static void fade_cb(lv_event_t *e)
{
    g_settings.alarm_fade = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ui_settings_changed();
}

static const uint8_t s_ring_vals[] = {5, 10, 15, 30, 60};

static void ring_cb(lv_event_t *e)
{
    g_settings.alarm_ring_min = s_ring_vals[lv_dropdown_get_selected(lv_event_get_target(e))];
    ui_settings_changed();
}

static lv_obj_t *slider_row(lv_obj_t *card, const char *name, int min, int max, int val, lv_obj_t **val_lbl,
                            const char *fmt, lv_event_cb_t cb)
{
    lv_obj_t *r = ui_row(card, name);
    lv_obj_set_style_min_height(r, 40, 0);
    *val_lbl = label(r, "", &lv_font_montserrat_20, UI_C_MUTED);
    lv_label_set_text_fmt(*val_lbl, fmt, val);
    lv_obj_t *s = lv_slider_create(card);
    lv_obj_set_width(s, lv_pct(100));
    lv_obj_set_height(s, 18);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_pad_ver(s, 12, 0);
    lv_obj_set_ext_click_area(s, 20);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return s;
}

static void alarm_card(lv_obj_t *parent, int slot)
{
    const alarm_t *a = &g_settings.alarms[slot];
    lv_obj_t *c = lv_obj_create(parent);
    lv_obj_set_size(c, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_style_bg_color(c, UI_C_SURFACE, 0);
    lv_obj_set_style_bg_color(c, UI_C_SURFACE2, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_border_color(c, a->on ? UI_C_BORDER : UI_C_SURFACE2, 0);
    lv_obj_set_style_radius(c, 18, 0);
    lv_obj_set_style_pad_hor(c, 22, 0);
    lv_obj_set_style_pad_ver(c, 12, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, edit_cb, LV_EVENT_CLICKED, (void *)(intptr_t)slot);

    lv_color_t fg = a->on ? UI_C_TEXT : UI_C_MUTED;
    char tm[24];
    alarm_fmt_time(tm, sizeof(tm), a->hour, a->minute);
    lv_obj_t *tl = label(c, tm, ui_font(UI_FW_MEDIUM, 46), fg);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 0, -4);

    char days[48], info[128];
    alarm_fmt_days(a, days, sizeof(days));
    snprintf(info, sizeof(info), "%s   %s %s%s%s", days, a->sound == ALARM_SND_RADIO ? LV_SYMBOL_AUDIO : LV_SYMBOL_BELL,
             alarm_sound_name(a->sound), a->label[0] ? "   " : "", a->label);
    lv_obj_t *il = label(c, info, &lv_font_montserrat_20, a->on ? UI_C_MUTED : lv_color_hex(0x56606E));
    lv_obj_set_width(il, 520);
    lv_label_set_long_mode(il, LV_LABEL_LONG_DOT);
    lv_obj_align(il, LV_ALIGN_TOP_LEFT, 2, 62);
    lv_obj_set_height(c, 128);

    lv_obj_t *sw = lv_switch_create(c);
    lv_obj_set_size(sw, 84, 44);
    lv_obj_set_state(sw, LV_STATE_CHECKED, a->on);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_ext_click_area(sw, 16);
    lv_obj_add_event_cb(sw, switch_cb, LV_EVENT_VALUE_CHANGED, (void *)(intptr_t)slot);
}

static void page_build(void)
{
    lv_obj_t *p = s_page;
    lv_obj_clean(p);

    lv_obj_t *hdr = lv_obj_create(p);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), 60);
    lv_obj_t *t = label(hdr, LV_SYMBOL_BELL, &lv_font_montserrat_28, UI_C_ACCENT2);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *t2 = label(hdr, "Alarms", ui_font(UI_FW_MEDIUM, 32), UI_C_TEXT);
    lv_obj_align(t2, LV_ALIGN_LEFT_MID, 46, 2);
    lv_obj_t *db = ui_button(hdr, "Done", UI_C_ACCENT);
    lv_obj_set_width(db, 120);
    lv_obj_align(db, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(db, done_cb, LV_EVENT_CLICKED, NULL);

    time_t until;
    if (alarm_snoozed(&until)) {
        lv_obj_t *c = ui_card(p);
        lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_set_style_border_color(c, UI_C_WARN, 0);
        char w[32], m[64];
        alarm_fmt_when(w, sizeof(w), until);
        snprintf(m, sizeof(m), "Snoozed until %s", w);
        label(c, m, &lv_font_montserrat_24, UI_C_WARN);
        lv_obj_t *b = ui_button(c, "Cancel snooze", UI_C_SURFACE2);
        lv_obj_add_event_cb(b, cancel_snooze_cb, LV_EVENT_CLICKED, NULL);
    }
    if (!alarm_valid_time()) {
        lv_obj_t *n = label(p, LV_SYMBOL_WARNING "  The clock is not set yet. Alarms ring once network time arrives.",
                            &lv_font_montserrat_20, UI_C_WARN);
        lv_obj_set_width(n, lv_pct(100));
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    }

    int used = 0;
    for (int i = 0; i < ALARM_MAX; i++) {
        if (!g_settings.alarms[i].used) continue;
        alarm_card(p, i);
        used++;
    }
    if (!used) {
        lv_obj_t *n = label(p, "No alarms yet. Tap New alarm to add one.", &lv_font_montserrat_20, UI_C_MUTED);
        lv_obj_set_style_pad_ver(n, 18, 0);
    }
    if (used < ALARM_MAX) {
        lv_obj_t *nb = ui_button(p, LV_SYMBOL_PLUS "  New alarm", UI_C_SURFACE2);
        lv_obj_set_width(nb, lv_pct(100));
        lv_obj_add_event_cb(nb, new_cb, LV_EVENT_CLICKED, NULL);
    }

    ui_section_title(p, "OPTIONS");
    lv_obj_t *c = ui_card(p);
    lv_obj_set_style_pad_row(c, 6, 0);
    slider_row(c, "Snooze length", 1, 10, g_settings.alarm_snooze_min, &s_snooze_val, "%d min", snooze_len_cb);
    slider_row(c, "Alarm volume", 10, 100, g_settings.alarm_volume, &s_vol_val, "%d %%", vol_cb);
    lv_obj_t *r = ui_row(c, "Gentle wake (volume rises over 30 s)");
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 84, 44);
    lv_obj_set_state(sw, LV_STATE_CHECKED, g_settings.alarm_fade);
    lv_obj_add_event_cb(sw, fade_cb, LV_EVENT_VALUE_CHANGED, NULL);
    r = ui_row(c, "Stop ringing after");
    lv_obj_t *dd = lv_dropdown_create(r);
    lv_dropdown_set_options(dd, "5 min\n10 min\n15 min\n30 min\n60 min");
    int sel = 3;
    for (int i = 0; i < 5; i++) if (s_ring_vals[i] == g_settings.alarm_ring_min) sel = i;
    lv_dropdown_set_selected(dd, sel);
    lv_obj_set_width(dd, 160);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(dd, UI_C_SURFACE2, 0);
    lv_obj_set_style_border_color(dd, UI_C_BORDER, 0);
    lv_obj_set_style_text_font(lv_dropdown_get_list(dd), &lv_font_montserrat_20, 0);
    lv_obj_add_event_cb(dd, ring_cb, LV_EVENT_VALUE_CHANGED, NULL);

    const char *name, *url;
    char st[128];
    if (alarm_station(&name, &url)) snprintf(st, sizeof(st), "Radio alarms play the last station: %s", name);
    else snprintf(st, sizeof(st), "Radio alarms: play a station in the Radio app first.");
    lv_obj_t *sl = label(c, st, &lv_font_montserrat_16, UI_C_MUTED);
    lv_obj_set_width(sl, lv_pct(100));
    lv_label_set_long_mode(sl, LV_LABEL_LONG_WRAP);
}

static bool s_refresh_pending;

static void refresh_async(void *arg)
{
    (void)arg;
    s_refresh_pending = false;
    if (!s_page) return;
    int32_t y = lv_obj_get_scroll_y(s_page);
    page_build();
    lv_obj_update_layout(s_page);
    lv_obj_scroll_to_y(s_page, y, LV_ANIM_OFF);
}

/* Rebuilt on the next LVGL cycle: it is usually called from inside an event of a widget on the page. */
void alarms_page_refresh(void)
{
    if (!s_page || s_refresh_pending) return;
    s_refresh_pending = true;
    lv_async_call(refresh_async, NULL);
}

static void page_deleted(lv_event_t *e)
{
    (void)e;
    s_page = NULL;
    s_snooze_val = s_vol_val = NULL;
}

void alarms_page_create(lv_obj_t *parent, void (*on_done)(void))
{
    s_on_done = on_done;
    s_page = lv_obj_create(parent);
    lv_obj_remove_style_all(s_page);
    lv_obj_set_size(s_page, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(s_page, 18, 0);
    lv_obj_set_style_pad_row(s_page, 14, 0);
    lv_obj_set_flex_flow(s_page, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_page, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_page, LV_SCROLLBAR_MODE_ACTIVE);
    lv_obj_add_event_cb(s_page, page_deleted, LV_EVENT_DELETE, NULL);
    page_build();
}

void alarms_page_destroy(void)
{
    if (s_ed) ed_close();
    s_page = NULL;
}
