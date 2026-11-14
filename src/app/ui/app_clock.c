#include <stdio.h>
#include "ui_internal.h"

static lv_obj_t *s_root;                 /* the app's content area */
static lv_obj_t *s_face;
static lv_obj_t *s_btn_dig, *s_btn_ana, *s_btn_24, *s_btn_sec, *s_btn_alarm, *s_alarm_lbl;
static lv_timer_t *s_timer;
static bool s_on_alarms;

static void show_clock(void);
static void show_alarms(void);

static void sync_buttons(void)
{
    if (!s_btn_dig) return;
    lv_obj_set_state(s_btn_dig, LV_STATE_CHECKED, g_settings.clock_face == CLOCK_FACE_DIGITAL);
    lv_obj_set_state(s_btn_ana, LV_STATE_CHECKED, g_settings.clock_face == CLOCK_FACE_ANALOG);
    lv_obj_set_state(s_btn_24, LV_STATE_CHECKED, g_settings.clock_24h);
    lv_obj_set_state(s_btn_sec, LV_STATE_CHECKED, g_settings.clock_seconds);
}

/* The alarm button doubles as the "alarm is set" indicator: it shows the next alarm time. */
static void sync_alarm_button(void)
{
    if (!s_alarm_lbl) return;
    char a[48];
    alarm_status_text(a, sizeof(a));
    bool snoozed = alarm_snoozed(NULL);
    time_t until;
    if (alarm_snoozed(&until)) {
        char w[32];
        alarm_fmt_when(w, sizeof(w), until);
        lv_label_set_text_fmt(s_alarm_lbl, LV_SYMBOL_BELL " z  %s", w);
    } else if (a[0]) {
        lv_label_set_text_fmt(s_alarm_lbl, LV_SYMBOL_BELL "  %s", a);
    } else {
        lv_label_set_text(s_alarm_lbl, LV_SYMBOL_BELL "  Alarms");
    }
    lv_obj_set_style_text_color(s_alarm_lbl, snoozed ? UI_C_WARN : a[0] ? UI_C_ACCENT2 : UI_C_MUTED, 0);
    lv_obj_set_style_border_color(s_btn_alarm, snoozed ? UI_C_WARN : a[0] ? UI_C_ACCENT2 : UI_C_SURFACE2, 0);
}

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    sync_alarm_button();
}

static void opt_cb(lv_event_t *e)
{
    int which = (int)(intptr_t)lv_event_get_user_data(e);
    switch (which) {
    case 0: g_settings.clock_face = CLOCK_FACE_DIGITAL; break;
    case 1: g_settings.clock_face = CLOCK_FACE_ANALOG; break;
    case 2: g_settings.clock_24h = !g_settings.clock_24h; break;
    case 3: g_settings.clock_seconds = !g_settings.clock_seconds; break;
    }
    sync_buttons();
    clock_face_rebuild(s_face);
    sync_alarm_button();
    ui_settings_changed();
}

static void show_alarms_async(void *arg) { (void)arg; show_alarms(); }
static void show_clock_async(void *arg) { (void)arg; show_clock(); }

static void alarms_cb(lv_event_t *e)
{
    (void)e;
    lv_async_call(show_alarms_async, NULL);   /* rebuilds the page this button lives on */
}

static void alarms_done(void)
{
    lv_async_call(show_clock_async, NULL);
}

static lv_obj_t *pill(lv_obj_t *parent, const char *text, int which, int width)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, width, 56);
    lv_obj_set_style_radius(b, 28, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, UI_C_SURFACE2, 0);
    lv_obj_set_style_bg_color(b, UI_C_ACCENT, LV_STATE_CHECKED);
    lv_obj_set_style_text_color(b, UI_C_MUTED, 0);
    lv_obj_set_style_text_color(b, lv_color_white(), LV_STATE_CHECKED);
    lv_obj_set_style_pad_hor(b, 6, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_20, 0);
    lv_obj_center(l);
    if (which >= 0) lv_obj_add_event_cb(b, opt_cb, LV_EVENT_CLICKED, (void *)(intptr_t)which);
    return b;
}

static void clear_root(void)
{
    if (s_on_alarms) alarms_page_destroy();
    lv_obj_clean(s_root);
    s_face = NULL;
    s_btn_dig = s_btn_ana = s_btn_24 = s_btn_sec = s_btn_alarm = s_alarm_lbl = NULL;
}

static void show_clock(void)
{
    if (!s_root) return;
    clear_root();
    s_on_alarms = false;
    lv_obj_t *parent = s_root;
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(parent, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *area = lv_obj_create(parent);
    lv_obj_remove_style_all(area);
    lv_obj_set_width(area, lv_pct(100));
    lv_obj_set_flex_grow(area, 1);
    lv_obj_remove_flag(area, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *bar = lv_obj_create(parent);
    lv_obj_remove_style_all(bar);
    lv_obj_set_size(bar, lv_pct(100), 84);
    lv_obj_set_flex_flow(bar, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(bar, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(bar, 10, 0);
    lv_obj_set_style_pad_bottom(bar, 14, 0);

    s_btn_dig = pill(bar, "Digital", 0, 108);
    s_btn_ana = pill(bar, "Analog", 1, 108);
    s_btn_24 = pill(bar, "24 h", 2, 84);
    s_btn_sec = pill(bar, "Seconds", 3, 116);
    s_btn_alarm = pill(bar, "", -1, 222);
    lv_obj_set_style_bg_color(s_btn_alarm, UI_C_SURFACE, 0);
    lv_obj_set_style_border_width(s_btn_alarm, 2, 0);
    s_alarm_lbl = lv_obj_get_child(s_btn_alarm, 0);
    lv_obj_set_width(s_alarm_lbl, 206);
    lv_label_set_long_mode(s_alarm_lbl, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(s_alarm_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_add_event_cb(s_btn_alarm, alarms_cb, LV_EVENT_CLICKED, NULL);
    sync_buttons();
    sync_alarm_button();

    lv_obj_update_layout(parent);   /* the face sizes itself from the area left above the bar */
    s_face = clock_face_create(area, false);
}

static void show_alarms(void)
{
    if (!s_root) return;
    clear_root();
    s_on_alarms = true;
    lv_obj_set_layout(s_root, LV_LAYOUT_NONE);
    alarms_page_create(s_root, alarms_done);
}

static void clock_create(lv_obj_t *parent)
{
    s_root = parent;
    s_on_alarms = false;
    show_clock();
    s_timer = lv_timer_create(timer_cb, 1000, NULL);
}

static void clock_destroy(void)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    if (s_on_alarms) alarms_page_destroy();
    s_on_alarms = false;
    s_root = NULL;
    s_face = NULL;
    s_btn_dig = s_btn_ana = s_btn_24 = s_btn_sec = s_btn_alarm = s_alarm_lbl = NULL;
}

static void clock_on_settings(void)
{
    if (s_on_alarms) {
        alarms_page_refresh();
        return;
    }
    sync_buttons();
    sync_alarm_button();
    if (s_face) clock_face_rebuild(s_face);
}

const ui_app_t app_clock = {
    .name = "Clock",
    .icon = &icon_clock,
    .create = clock_create,
    .destroy = clock_destroy,
    .on_settings = clock_on_settings,
};
