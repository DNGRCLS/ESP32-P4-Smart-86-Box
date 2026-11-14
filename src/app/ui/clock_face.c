/*
 * Clock face widget: digital or analog, sized to its parent.
 * Used by the Clock app and by the screensaver.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_internal.h"
#include "../core/tz_table.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

typedef struct {
    lv_obj_t *root;
    bool fullscreen;
    int last_sec;
    int last_min;
    /* digital */
    lv_obj_t *d_time, *d_ampm, *d_sec, *d_date, *d_zone;
    /* analog */
    lv_obj_t *a_dial, *a_hour, *a_min, *a_sec, *a_date, *a_ampm;
    lv_point_precise_t p_hour[2], p_min[2], p_sec[2];
    int32_t dia;
    int32_t inset;
    lv_timer_t *timer;
} face_t;

static const char *s_hour_labels[] = {"12", "1", "2", "3", "4", "5", "6", "7", "8", "9", "10", "11", NULL};

/* ------------------------------------------------------------------ text helpers */

static void zone_text(char *buf, size_t n, const struct tm *t)
{
    const tz_entry_t *z = tz_find(g_settings.tz_id);
    long off = ui_utc_offset(t);
    char sign = off < 0 ? '-' : '+';
    long a = labs(off);
    char abbr[16] = "";
    strftime(abbr, sizeof(abbr), "%Z", t);
    if (abbr[0] == '<' || abbr[0] == '+' || abbr[0] == '-') abbr[0] = 0;   /* numeric zones */
    if (g_settings.tz_custom[0] || !z) {
        snprintf(buf, n, "%s  UTC%c%02ld:%02ld", abbr, sign, a / 3600, (a % 3600) / 60);
    } else {
        snprintf(buf, n, "%s   %s  UTC%c%02ld:%02ld", z->city, abbr, sign, a / 3600, (a % 3600) / 60);
    }
}

static void squeeze_spaces(char *buf)
{
    char *dbl;
    while ((dbl = strstr(buf, "  ")) != NULL) memmove(dbl, dbl + 1, strlen(dbl));
    if (buf[0] == ' ') memmove(buf, buf + 1, strlen(buf));
}

/* ------------------------------------------------------------------ digital */

static void build_digital(face_t *f)
{
    lv_obj_t *r = f->root;
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(r, 0, 0);

    int big = f->fullscreen ? 216 : 196;

    lv_obj_t *row = lv_obj_create(r);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_add_flag(row, LV_OBJ_FLAG_EVENT_BUBBLE);

    f->d_time = lv_label_create(row);
    lv_obj_set_style_text_font(f->d_time, ui_font(UI_FW_LIGHT, big), 0);
    lv_obj_set_style_text_color(f->d_time, UI_C_TEXT, 0);
    lv_obj_set_style_text_letter_space(f->d_time, -2, 0);

    lv_obj_t *side = lv_obj_create(row);
    lv_obj_remove_style_all(side);
    lv_obj_set_size(side, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(side, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_top(side, big * 26 / 100, 0);
    lv_obj_set_style_pad_row(side, 0, 0);
    lv_obj_add_flag(side, LV_OBJ_FLAG_EVENT_BUBBLE);

    f->d_ampm = lv_label_create(side);
    lv_obj_set_style_text_font(f->d_ampm, ui_font(UI_FW_MEDIUM, 42), 0);
    lv_obj_set_style_text_color(f->d_ampm, UI_C_MUTED, 0);
    f->d_sec = lv_label_create(side);
    lv_obj_set_style_text_font(f->d_sec, ui_font(UI_FW_REGULAR, 42), 0);
    lv_obj_set_style_text_color(f->d_sec, UI_C_ACCENT2, 0);

    f->d_date = lv_label_create(r);
    lv_obj_set_style_text_font(f->d_date, ui_font(UI_FW_REGULAR, 36), 0);
    lv_obj_set_style_text_color(f->d_date, UI_C_TEXT, 0);
    lv_obj_set_style_margin_top(f->d_date, -big / 9, 0);

    f->d_zone = lv_label_create(r);
    lv_obj_set_style_text_align(f->d_zone, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_line_space(f->d_zone, 8, 0);
    lv_obj_set_style_text_font(f->d_zone, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(f->d_zone, UI_C_MUTED, 0);
    lv_obj_set_style_margin_top(f->d_zone, 22, 0);
}

static void update_digital(face_t *f, const struct tm *t, bool minute_changed)
{
    char buf[96];
    if (g_settings.clock_24h) {
        snprintf(buf, sizeof(buf), "%02d:%02d", t->tm_hour, t->tm_min);
        lv_obj_add_flag(f->d_ampm, LV_OBJ_FLAG_HIDDEN);
    } else {
        int h = t->tm_hour % 12;
        if (h == 0) h = 12;
        snprintf(buf, sizeof(buf), "%d:%02d", h, t->tm_min);
        lv_obj_remove_flag(f->d_ampm, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text(f->d_ampm, t->tm_hour < 12 ? "AM" : "PM");
    }
    lv_label_set_text(f->d_time, buf);
    if (g_settings.clock_seconds) {
        lv_obj_remove_flag(f->d_sec, LV_OBJ_FLAG_HIDDEN);
        lv_label_set_text_fmt(f->d_sec, "%02d", t->tm_sec);
    } else {
        lv_obj_add_flag(f->d_sec, LV_OBJ_FLAG_HIDDEN);
    }
    if (minute_changed) {
        strftime(buf, sizeof(buf), "%A, %B %e, %Y", t);
        squeeze_spaces(buf);
        lv_label_set_text(f->d_date, buf);
        net_status_t st;
        plat_net_get_status(&st);
        char z[96];
        zone_text(z, sizeof(z), t);
        if (!st.time_synced) {
            snprintf(buf, sizeof(buf), "%s\n" LV_SYMBOL_WARNING "  waiting for network time", z);
            lv_label_set_text(f->d_zone, buf);
        } else {
            lv_label_set_text(f->d_zone, z);
        }
    }
}

/* ------------------------------------------------------------------ analog */

static lv_obj_t *make_hand(face_t *f, int width, lv_color_t color)
{
    lv_obj_t *l = lv_line_create(f->a_dial);
    lv_obj_set_style_line_width(l, width, 0);
    lv_obj_set_style_line_color(l, color, 0);
    lv_obj_set_style_line_rounded(l, true, 0);
    lv_obj_set_style_shadow_width(l, 0, 0);
    lv_obj_add_flag(l, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_add_flag(l, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_set_pos(l, -f->inset, -f->inset);
    lv_obj_set_size(l, f->dia, f->dia);
    return l;
}

static void build_analog(face_t *f)
{
    lv_obj_t *r = f->root;
    lv_obj_update_layout(r);
    int32_t w = lv_obj_get_content_width(r), h = lv_obj_get_content_height(r);
    f->dia = (w < h ? w : h) - (f->fullscreen ? 30 : 36);
    int32_t D = f->dia;

    lv_obj_t *dial = lv_scale_create(r);
    f->a_dial = dial;
    lv_obj_set_size(dial, D, D);
    lv_obj_center(dial);
    lv_obj_add_flag(dial, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_remove_flag(dial, LV_OBJ_FLAG_CLICKABLE);
    lv_scale_set_mode(dial, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_total_tick_count(dial, 60);
    lv_scale_set_major_tick_every(dial, 5);
    lv_scale_set_range(dial, 0, 59);
    lv_scale_set_angle_range(dial, 354);
    lv_scale_set_rotation(dial, 270);
    lv_scale_set_label_show(dial, true);
    lv_scale_set_text_src(dial, s_hour_labels);

    lv_obj_set_style_radius(dial, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dial, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dial, lv_color_hex(0x151B23), 0);
    lv_obj_set_style_border_width(dial, 6, 0);
    lv_obj_set_style_border_color(dial, lv_color_hex(0x2A3442), 0);
    lv_obj_set_style_pad_all(dial, D / 30, 0);
    f->inset = 6 + D / 30;   /* children are placed relative to the content box */
    lv_obj_set_style_clip_corner(dial, true, 0);

    /* major ticks + numerals */
    lv_obj_set_style_length(dial, D / 18, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(dial, 6, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(dial, UI_C_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_text_font(dial, ui_font(UI_FW_MEDIUM, D / 12), LV_PART_INDICATOR);
    lv_obj_set_style_text_color(dial, UI_C_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_pad_radial(dial, D / 16, LV_PART_INDICATOR);
    /* minute ticks */
    lv_obj_set_style_length(dial, D / 40, LV_PART_ITEMS);
    lv_obj_set_style_line_width(dial, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(dial, UI_C_MUTED, LV_PART_ITEMS);
    /* no arc line */
    lv_obj_set_style_arc_width(dial, 0, LV_PART_MAIN);

    f->a_date = lv_label_create(dial);
    lv_obj_add_flag(f->a_date, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_style_text_font(f->a_date, ui_font(UI_FW_MEDIUM, D / 20), 0);
    lv_obj_set_style_text_color(f->a_date, UI_C_ACCENT2, 0);
    lv_obj_set_style_bg_color(f->a_date, lv_color_hex(0x0B0E13), 0);
    lv_obj_set_style_bg_opa(f->a_date, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(f->a_date, 8, 0);
    lv_obj_set_style_pad_hor(f->a_date, 12, 0);
    lv_obj_set_style_pad_ver(f->a_date, 2, 0);
    lv_obj_set_style_border_width(f->a_date, 1, 0);
    lv_obj_set_style_border_color(f->a_date, UI_C_BORDER, 0);
    lv_obj_align(f->a_date, LV_ALIGN_CENTER, 0, D * 17 / 100);

    f->a_hour = make_hand(f, D / 34, UI_C_TEXT);
    f->a_min = make_hand(f, D / 52, UI_C_TEXT);
    f->a_sec = make_hand(f, 3, UI_C_ACCENT2);

    lv_obj_t *cap = lv_obj_create(dial);
    lv_obj_remove_style_all(cap);
    lv_obj_add_flag(cap, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_size(cap, D / 22, D / 22);
    lv_obj_set_style_radius(cap, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(cap, UI_C_ACCENT2, 0);
    lv_obj_set_style_bg_opa(cap, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cap, 3, 0);
    lv_obj_set_style_border_color(cap, UI_C_TEXT, 0);
    lv_obj_center(cap);
}

static void set_hand(lv_obj_t *line, lv_point_precise_t *pts, int32_t D, double angle_deg, double len,
                     double tail)
{
    double a = angle_deg * M_PI / 180.0;
    double cx = D / 2.0, cy = D / 2.0;
    pts[0].x = (lv_value_precise_t)(cx - tail * sin(a));
    pts[0].y = (lv_value_precise_t)(cy + tail * cos(a));
    pts[1].x = (lv_value_precise_t)(cx + len * sin(a));
    pts[1].y = (lv_value_precise_t)(cy - len * cos(a));
    lv_line_set_points(line, pts, 2);
}

static void update_analog(face_t *f, const struct tm *t, bool minute_changed)
{
    int32_t D = f->dia;
    double R = D / 2.0;
    double sec = t->tm_sec;
    double min = t->tm_min + sec / 60.0;
    double hr = (t->tm_hour % 12) + min / 60.0;
    /* the dial is drawn inside the border + padding: keep hands within */
    set_hand(f->a_hour, f->p_hour, D, hr * 30.0, R * 0.50, R * 0.08);
    set_hand(f->a_min, f->p_min, D, min * 6.0, R * 0.74, R * 0.08);
    if (g_settings.clock_seconds) {
        lv_obj_remove_flag(f->a_sec, LV_OBJ_FLAG_HIDDEN);
        set_hand(f->a_sec, f->p_sec, D, sec * 6.0, R * 0.80, R * 0.18);
    } else {
        lv_obj_add_flag(f->a_sec, LV_OBJ_FLAG_HIDDEN);
    }
    if (minute_changed) {
        char buf[32];
        strftime(buf, sizeof(buf), "%a %e", t);
        squeeze_spaces(buf);
        for (char *p = buf; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
        if (!g_settings.clock_24h) {
            size_t l = strlen(buf);
            snprintf(buf + l, sizeof(buf) - l, "  %s", t->tm_hour < 12 ? "AM" : "PM");
        }
        lv_label_set_text(f->a_date, buf);
    }
}

/* ------------------------------------------------------------------ common */

static void face_update(face_t *f, bool force)
{
    time_t now = plat_now();
    struct tm t;
    localtime_r(&now, &t);
    if (!force && t.tm_sec == f->last_sec) return;
    bool minute_changed = force || t.tm_min != f->last_min;
    f->last_sec = t.tm_sec;
    f->last_min = t.tm_min;
    if (g_settings.clock_face == CLOCK_FACE_ANALOG) update_analog(f, &t, minute_changed);
    else update_digital(f, &t, minute_changed);
}

static void face_timer_cb(lv_timer_t *tm)
{
    face_update((face_t *)lv_timer_get_user_data(tm), false);
}

static void face_delete_cb(lv_event_t *e)
{
    face_t *f = (face_t *)lv_event_get_user_data(e);
    if (f->timer) lv_timer_delete(f->timer);
    free(f);
}

static void face_build(face_t *f)
{
    lv_obj_clean(f->root);
    f->d_time = f->d_ampm = f->d_sec = f->d_date = f->d_zone = NULL;
    f->a_dial = f->a_hour = f->a_min = f->a_sec = f->a_date = f->a_ampm = NULL;
    lv_obj_set_layout(f->root, LV_LAYOUT_NONE);
    if (g_settings.clock_face == CLOCK_FACE_ANALOG) build_analog(f);
    else build_digital(f);
    f->last_sec = -1;
    f->last_min = -1;
    face_update(f, true);
}

lv_obj_t *clock_face_create(lv_obj_t *parent, bool fullscreen)
{
    face_t *f = calloc(1, sizeof(face_t));
    f->fullscreen = fullscreen;
    f->root = lv_obj_create(parent);
    lv_obj_remove_style_all(f->root);
    lv_obj_set_size(f->root, lv_pct(100), lv_pct(100));
    lv_obj_remove_flag(f->root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(f->root, f);
    lv_obj_add_event_cb(f->root, face_delete_cb, LV_EVENT_DELETE, f);
    face_build(f);
    f->timer = lv_timer_create(face_timer_cb, 200, f);
    return f->root;
}

void clock_face_rebuild(lv_obj_t *face)
{
    face_t *f = (face_t *)lv_obj_get_user_data(face);
    if (f) face_build(f);
}
