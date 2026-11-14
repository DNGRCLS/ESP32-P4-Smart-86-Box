/* Calendar app: month grid with Canadian statutory holidays and observances. */
#include <stdio.h>
#include <string.h>
#include "ui_internal.h"
#include "../core/holidays.h"
#include "../core/tz_table.h"

#define MAX_HOL 80

static lv_obj_t *s_title, *s_grid, *s_list, *s_list_hdr, *s_prov_dd;
static lv_obj_t *s_cells[42];
static int s_year, s_month, s_sel_day;
static holiday_t s_hol[MAX_HOL];
static int s_hol_n, s_hol_year = -1;
static char s_hol_prov[4];
static bool s_hol_dst;
static lv_timer_t *s_timer;
static int s_today_y, s_today_m, s_today_d;

static const char *const s_months[] = {"January", "February", "March", "April", "May", "June", "July",
                                       "August", "September", "October", "November", "December"};
static const char *const s_wd_short[] = {"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"};

static lv_color_t kind_color(int k)
{
    switch (k) {
    case HOL_STAT: return UI_C_ERR;
    case HOL_FEDERAL: return UI_C_ACCENT;
    case HOL_OPTIONAL: return UI_C_WARN;
    default: return UI_C_MUTED;
    }
}

static const char *kind_text(int k)
{
    switch (k) {
    case HOL_STAT: return "Statutory";
    case HOL_FEDERAL: return "Federal";
    case HOL_OPTIONAL: return "Optional";
    default: return "Observance";
    }
}

static bool zone_has_na_dst(void)
{
    const tz_entry_t *z = tz_find(g_settings.tz_id);
    return !g_settings.tz_custom[0] && g_settings.dst_mode == DST_AUTO && z && z->dst_rule &&
           strcmp(z->dst_rule, "M3.2.0,M11.1.0") == 0;
}

static void ensure_holidays(void)
{
    bool dst = zone_has_na_dst();
    if (s_hol_year == s_year && !strcmp(s_hol_prov, g_settings.province) && s_hol_dst == dst) return;
    s_hol_n = holidays_for_year(s_year, g_settings.province, dst, s_hol, MAX_HOL);
    s_hol_year = s_year;
    s_hol_dst = dst;
    snprintf(s_hol_prov, sizeof(s_hol_prov), "%s", g_settings.province);
}

static void update_today(void)
{
    time_t now = plat_now();
    struct tm t;
    localtime_r(&now, &t);
    s_today_y = t.tm_year + 1900;
    s_today_m = t.tm_mon + 1;
    s_today_d = t.tm_mday;
}

static void fill_list(void)
{
    lv_obj_clean(s_list);
    char hdr[64];
    if (s_sel_day) {
        int wd = day_of_week(s_year, s_month, s_sel_day);
        static const char *const wl[] = {"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday", "Saturday"};
        snprintf(hdr, sizeof(hdr), "%s, %s %d", wl[wd], s_months[s_month - 1], s_sel_day);
    } else {
        snprintf(hdr, sizeof(hdr), "%s %d  -  %s", s_months[s_month - 1], s_year, province_name(g_settings.province));
    }
    lv_label_set_text(s_list_hdr, hdr);
    int shown = 0;
    for (int i = 0; i < s_hol_n; i++) {
        holiday_t *h = &s_hol[i];
        if (h->month != s_month) continue;
        if (s_sel_day && h->day != s_sel_day) continue;
        lv_obj_t *r = lv_obj_create(s_list);
        lv_obj_remove_style_all(r);
        lv_obj_set_size(r, lv_pct(100), 36);
        lv_obj_t *dt = lv_label_create(r);
        lv_label_set_text_fmt(dt, "%s %d", s_wd_short[day_of_week(s_year, h->month, h->day)], h->day);
        lv_obj_set_style_text_color(dt, UI_C_MUTED, 0);
        lv_obj_align(dt, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *nm = lv_label_create(r);
        lv_label_set_text(nm, h->name);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_size(nm, 450, 26);
        lv_obj_set_style_text_font(nm, ui_font(UI_FW_REGULAR, 20), 0);
        lv_obj_align(nm, LV_ALIGN_LEFT_MID, 84, 0);
        lv_obj_t *k = lv_label_create(r);
        lv_label_set_text(k, kind_text(h->kind));
        lv_obj_set_style_text_font(k, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(k, kind_color(h->kind), 0);
        lv_obj_set_style_border_color(k, kind_color(h->kind), 0);
        lv_obj_set_style_border_width(k, 1, 0);
        lv_obj_set_style_radius(k, 10, 0);
        lv_obj_set_style_pad_hor(k, 8, 0);
        lv_obj_set_style_pad_ver(k, 2, 0);
        lv_obj_align(k, LV_ALIGN_RIGHT_MID, 0, 0);
        shown++;
    }
    if (!shown) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_label_set_text(l, s_sel_day ? "No holidays on this day" : "No holidays this month");
        lv_obj_set_style_text_color(l, UI_C_MUTED, 0);
    }
}

static void render(void)
{
    ensure_holidays();
    update_today();
    lv_label_set_text_fmt(s_title, "%s %d", s_months[s_month - 1], s_year);
    int first = day_of_week(s_year, s_month, 1);
    int dim = days_in_month(s_year, s_month);
    for (int i = 0; i < 42; i++) {
        lv_obj_t *c = s_cells[i];
        int day = i - first + 1;
        lv_obj_t *num = lv_obj_get_child(c, 0);
        lv_obj_t *dot = lv_obj_get_child(c, 1);
        if (day < 1 || day > dim) {
            /* keep the cell in the flex layout so weekdays stay aligned, just invisible */
            lv_obj_set_style_opa(c, LV_OPA_TRANSP, 0);
            lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
            continue;
        }
        lv_obj_set_style_opa(c, LV_OPA_COVER, 0);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_user_data(c, (void *)(intptr_t)day);
        lv_label_set_text_fmt(num, "%d", day);
        bool today = s_year == s_today_y && s_month == s_today_m && day == s_today_d;
        int best = 99;
        for (int h = 0; h < s_hol_n; h++)
            if (s_hol[h].month == s_month && s_hol[h].day == day && s_hol[h].kind < best) best = s_hol[h].kind;
        lv_obj_set_style_bg_color(c, best == HOL_STAT ? lv_color_hex(0x3A1A1E) : UI_C_SURFACE, 0);
        lv_obj_set_style_border_color(c, day == s_sel_day ? UI_C_TEXT : today ? UI_C_ACCENT : UI_C_BORDER, 0);
        lv_obj_set_style_border_width(c, day == s_sel_day || today ? 2 : 1, 0);
        lv_obj_set_style_bg_color(num, UI_C_ACCENT, 0);
        lv_obj_set_style_bg_opa(num, today ? LV_OPA_COVER : LV_OPA_TRANSP, 0);
        lv_obj_set_style_text_color(num, today ? lv_color_white() : (i % 7 == 0 || i % 7 == 6) ? UI_C_MUTED : UI_C_TEXT, 0);
        if (best <= HOL_OBSERVANCE) {
            lv_obj_remove_flag(dot, LV_OBJ_FLAG_HIDDEN);
            lv_obj_set_style_bg_color(dot, kind_color(best), 0);
        } else {
            lv_obj_add_flag(dot, LV_OBJ_FLAG_HIDDEN);
        }
    }
    /* hide the 6th row when the month fits in 5 */
    fill_list();
}

static void nav_cb(lv_event_t *e)
{
    int d = (int)(intptr_t)lv_event_get_user_data(e);
    if (d == 0) {
        update_today();
        s_year = s_today_y;
        s_month = s_today_m;
        s_sel_day = s_today_d;
    } else {
        s_month += d;
        if (s_month < 1) { s_month = 12; s_year--; }
        if (s_month > 12) { s_month = 1; s_year++; }
        s_sel_day = 0;
    }
    render();
}

static void cell_cb(lv_event_t *e)
{
    int day = (int)(intptr_t)lv_obj_get_user_data(lv_event_get_current_target(e));
    s_sel_day = s_sel_day == day ? 0 : day;
    render();
}

static void prov_cb(lv_event_t *e)
{
    int i = lv_dropdown_get_selected(lv_event_get_target(e));
    snprintf(g_settings.province, sizeof(g_settings.province), "%s", g_province_codes[i]);
    ui_settings_changed();
    render();
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    int d = s_today_d;
    update_today();
    if (d != s_today_d) render();   /* midnight */
}

static lv_obj_t *nav_btn(lv_obj_t *p, const char *txt, int d)
{
    lv_obj_t *b = ui_button(p, txt, UI_C_SURFACE2);
    lv_obj_set_size(b, d == 0 ? 100 : 60, 52);
    lv_obj_set_style_pad_hor(b, 0, 0);
    lv_obj_add_event_cb(b, nav_cb, LV_EVENT_CLICKED, (void *)(intptr_t)d);
    return b;
}

static void cal_create(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 12, 0);
    lv_obj_set_style_pad_row(parent, 8, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(parent);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), 56);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 8, 0);
    nav_btn(hdr, LV_SYMBOL_LEFT, -1);
    s_title = lv_label_create(hdr);
    lv_obj_set_width(s_title, 250);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_title, ui_font(UI_FW_MEDIUM, 30), 0);
    nav_btn(hdr, LV_SYMBOL_RIGHT, 1);
    nav_btn(hdr, "Today", 0);

    static char opts[512];
    opts[0] = 0;
    int sel = 8;
    for (int i = 0; i < 13; i++) {
        if (i) strcat(opts, "\n");
        strcat(opts, g_province_codes[i]);
        if (!strcmp(g_province_codes[i], g_settings.province)) sel = i;
    }
    s_prov_dd = lv_dropdown_create(hdr);
    lv_dropdown_set_options(s_prov_dd, opts);
    lv_dropdown_set_selected(s_prov_dd, sel);
    lv_obj_set_size(s_prov_dd, 110, 52);
    lv_obj_set_style_bg_color(s_prov_dd, UI_C_SURFACE2, 0);
    lv_obj_add_event_cb(s_prov_dd, prov_cb, LV_EVENT_VALUE_CHANGED, NULL);

    lv_obj_t *wk = lv_obj_create(parent);
    lv_obj_remove_style_all(wk);
    lv_obj_set_size(wk, lv_pct(100), 24);
    lv_obj_set_flex_flow(wk, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(wk, 4, 0);
    for (int i = 0; i < 7; i++) {
        lv_obj_t *l = lv_label_create(wk);
        lv_label_set_text(l, s_wd_short[i]);
        lv_obj_set_width(l, 96);
        lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l, UI_C_MUTED, 0);
    }

    s_grid = lv_obj_create(parent);
    lv_obj_remove_style_all(s_grid);
    lv_obj_set_size(s_grid, lv_pct(100), 6 * 54 + 5 * 4);
    lv_obj_set_flex_flow(s_grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(s_grid, 4, 0);
    lv_obj_set_style_pad_row(s_grid, 4, 0);
    for (int i = 0; i < 42; i++) {
        lv_obj_t *c = lv_obj_create(s_grid);
        s_cells[i] = c;
        lv_obj_set_size(c, 96, 54);
        lv_obj_set_style_radius(c, 10, 0);
        lv_obj_set_style_pad_all(c, 0, 0);
        lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(c, cell_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_t *n = lv_label_create(c);
        lv_obj_set_style_text_font(n, ui_font(UI_FW_REGULAR, 22), 0);
        lv_obj_set_size(n, 38, 38);
        lv_obj_set_style_radius(n, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_text_align(n, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_pad_top(n, 5, 0);
        lv_obj_align(n, LV_ALIGN_CENTER, 0, -4);
        lv_obj_t *dot = lv_obj_create(c);
        lv_obj_remove_style_all(dot);
        lv_obj_set_size(dot, 26, 5);
        lv_obj_set_style_radius(dot, 3, 0);
        lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
        lv_obj_align(dot, LV_ALIGN_BOTTOM_MID, 0, -4);
    }

    s_list_hdr = lv_label_create(parent);
    lv_obj_set_style_text_font(s_list_hdr, ui_font(UI_FW_MEDIUM, 20), 0);
    lv_obj_set_style_text_color(s_list_hdr, UI_C_MUTED, 0);
    s_list = lv_obj_create(parent);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);

    update_today();
    s_year = s_today_y;
    s_month = s_today_m;
    s_sel_day = 0;
    s_hol_year = -1;
    render();
    s_timer = lv_timer_create(tick_cb, 30000, NULL);
}

static void cal_destroy(void)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    s_title = s_grid = s_list = s_list_hdr = s_prov_dd = NULL;
}

static void cal_on_settings(void)
{
    if (!s_grid) return;
    for (int i = 0; i < 13; i++)
        if (!strcmp(g_province_codes[i], g_settings.province)) lv_dropdown_set_selected(s_prov_dd, i);
    render();
}

void app_calendar_debug_goto(int y, int m, int d) { s_year = y; s_month = m; s_sel_day = d; render(); }

const ui_app_t app_calendar = {
    .name = "Calendar",
    .icon = &icon_calendar,
    .create = cal_create,
    .destroy = cal_destroy,
    .on_settings = cal_on_settings,
};
