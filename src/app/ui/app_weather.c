/*
 * Weather app (OpenWeather).
 *
 *   main view : place, now, alert, next hours, details tiles, daily forecast
 *   settings  : API key (+ QR codes to OpenWeather sign-up), units, air-quality scale, locations
 *   setup     : shown until an API key is entered
 *
 * Data comes from plat_weather_*() (fetched in the background); everything is stored in metric
 * and converted here for display.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_internal.h"

#define URL_SIGNUP  "https://home.openweathermap.org/users/sign_up"
#define URL_KEYS    "https://home.openweathermap.org/api_keys"
#define URL_ONECALL "https://openweathermap.org/price#one_call"

static lv_obj_t *s_root;
static lv_timer_t *s_timer;
static uint32_t s_gen_seen;
static bool s_busy_seen;
static enum { V_MAIN, V_SETTINGS } s_view;
static weather_t *s_w;              /* snapshot being shown (PSRAM-sized struct, heap) */
static lv_obj_t *s_refresh_btn;

static void show(int view);
static void go(int view);   /* rebuild on the next LVGL cycle: safe from inside a widget event */

/* ================================================================== units & formatting */

static bool imperial(void) { return g_settings.wx_units == WX_IMPERIAL; }

static float conv_t(float c) { return imperial() ? c * 9.0f / 5.0f + 32.0f : c; }

static void fmt_temp(char *b, size_t n, float c)
{
    if (c == WX_NA) snprintf(b, n, "-");
    else snprintf(b, n, "%d\xC2\xB0", (int)lroundf(conv_t(c)));
}

static void fmt_wind(char *b, size_t n, float ms)
{
    if (ms == WX_NA) { snprintf(b, n, "-"); return; }
    if (imperial()) snprintf(b, n, "%d mph", (int)lroundf(ms * 2.23694f));
    else snprintf(b, n, "%d km/h", (int)lroundf(ms * 3.6f));
}

static void fmt_precip(char *b, size_t n, float mm)
{
    if (imperial()) snprintf(b, n, "%.2f in", mm / 25.4f);
    else if (mm < 10) snprintf(b, n, "%.1f mm", mm);
    else snprintf(b, n, "%.0f mm", mm);
}

/* times are shown in the location's own time zone */
static void loc_tm(time_t t, struct tm *out)
{
    time_t x = t + (s_w ? s_w->tz_offset : 0);
    gmtime_r(&x, out);
}

static void fmt_clock(char *b, size_t n, time_t t)
{
    if (!t) { snprintf(b, n, "-"); return; }
    struct tm tm;
    loc_tm(t, &tm);
    ui_fmt_time(b, n, &tm, false, true);
}

static void fmt_hour_short(char *b, size_t n, time_t t)
{
    struct tm tm;
    loc_tm(t, &tm);
    if (g_settings.clock_24h) snprintf(b, n, "%02d:00", tm.tm_hour);
    else snprintf(b, n, "%d %s", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12, tm.tm_hour < 12 ? "AM" : "PM");
}

const lv_image_dsc_t *wx_icon_img(const char *code, bool large)
{
#define PICK(c) return large ? &wx_##c##_l : &wx_##c##_s
    bool night = code && code[0] && code[2] == 'n';
    int k = code && code[0] ? atoi(code) : 3;
    switch (k) {
    case 1: if (night) PICK(01n); PICK(01d);
    case 2: if (night) PICK(02n); PICK(02d);
    case 3: PICK(03);
    case 4: PICK(04);
    case 9: PICK(09);
    case 10: if (night) PICK(10n); PICK(10d);
    case 11: PICK(11);
    case 13: PICK(13);
    case 50: PICK(50);
    default: PICK(03);
    }
#undef PICK
}

/* Air quality in the chosen scale: value text, label, colour */
static void aq_text(const weather_t *w, char *val, size_t vn, char *lbl, size_t ln, lv_color_t *col)
{
    *col = UI_C_TEXT;
    if (!w->aq_valid) { snprintf(val, vn, "-"); snprintf(lbl, ln, "No data"); return; }
    static const uint32_t c5[] = {0x3FB950, 0x3FB950, 0xC9D13A, 0xE3B341, 0xF0883E, 0xF85149};
    switch (g_settings.wx_aq_scale) {
    case WX_AQ_US: {
        int a = wx_us_aqi(w->pm2_5, w->pm10, w->o3);
        snprintf(val, vn, "%d", a);
        snprintf(lbl, ln, "%s (US AQI)", wx_us_aqi_label(a));
        *col = lv_color_hex(a <= 50 ? c5[1] : a <= 100 ? c5[2] : a <= 150 ? c5[3] : a <= 200 ? c5[4] : c5[5]);
        break;
    }
    case WX_AQ_OWM:
        snprintf(val, vn, "%d", w->owm_aqi);
        snprintf(lbl, ln, "%s (1-5)", wx_owm_aqi_label(w->owm_aqi));
        *col = lv_color_hex(c5[w->owm_aqi >= 0 && w->owm_aqi <= 5 ? w->owm_aqi : 0]);
        break;
    default: {
        int a = wx_aqhi(w->o3, w->no2, w->pm2_5);
        if (a > 10) snprintf(val, vn, "10+");
        else snprintf(val, vn, "%d", a);
        snprintf(lbl, ln, "%s (AQHI)", wx_aqhi_risk(a));
        *col = lv_color_hex(a <= 3 ? c5[1] : a <= 6 ? c5[3] : a <= 10 ? c5[4] : c5[5]);
        break;
    }
    }
}

/* "Feels like" with the reason: humidex / heat index / wind chill */
static void feels_text(const weather_t *w, char *val, size_t vn, char *why, size_t wn)
{
    float t = w->temp;
    why[0] = 0;
    if (t == WX_NA) { snprintf(val, vn, "-"); return; }
    float kmh = w->wind_ms == WX_NA ? 0 : w->wind_ms * 3.6f;
    if (wx_wind_chill_applies(t, kmh)) {
        fmt_temp(val, vn, wx_wind_chill(t, kmh));
        snprintf(why, wn, "Wind chill");
        return;
    }
    if (!imperial() && w->dew_point != WX_NA) {
        float h = wx_humidex(t, w->dew_point);
        if (wx_humidex_applies(t, h)) {
            snprintf(val, vn, "%d", (int)lroundf(h));   /* humidex has no unit */
            snprintf(why, wn, "Humidex");
            return;
        }
    }
    if (imperial() && w->humidity != WX_NA) {
        float hi = wx_heat_index_c(t, w->humidity);
        if (wx_heat_index_applies(t, hi)) {
            fmt_temp(val, vn, hi);
            snprintf(why, wn, "Heat index");
            return;
        }
    }
    fmt_temp(val, vn, w->feels != WX_NA ? w->feels : t);
    snprintf(why, wn, "Feels like");
}

/* The menus use the built-in Montserrat font (ASCII only): "Montréal" -> "Montreal". */
static void ascii_fold(char *dst, size_t n, const char *src)
{
    static const char *map = "AAAAAAACEEEEIIIIDNOOOOOxOUUUUYPsaaaaaaaceeeeiiiidnooooo/ouuuuypy";   /* U+00C0..U+00FF */
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)src; *p && o + 1 < n; p++) {
        if (*p < 0x80) dst[o++] = (char)*p;
        else if (*p == 0xC3 && p[1] >= 0x80 && p[1] <= 0xBF) { dst[o++] = map[p[1] - 0x80 + 0]; p++; }
        else if ((*p & 0xE0) == 0xC0 && p[1]) { dst[o++] = '?'; p++; }
        else if ((*p & 0xF0) == 0xE0 && p[1] && p[2]) { dst[o++] = '?'; p += 2; }
        else if ((*p & 0xF8) == 0xF0 && p[1] && p[2] && p[3]) { dst[o++] = '?'; p += 3; }
    }
    dst[o] = 0;
}

/* ================================================================== widgets */

static lv_obj_t *lbl(lv_obj_t *p, const char *t, const lv_font_t *f, lv_color_t c)
{
    lv_obj_t *l = lv_label_create(p);
    lv_label_set_text(l, t);
    lv_obj_set_style_text_font(l, f, 0);
    lv_obj_set_style_text_color(l, c, 0);
    return l;
}

static lv_obj_t *col(lv_obj_t *p, int gap)
{
    lv_obj_t *c = lv_obj_create(p);
    lv_obj_remove_style_all(c);
    lv_obj_set_size(c, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(c, gap, 0);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(c, LV_OBJ_FLAG_EVENT_BUBBLE);
    return c;
}

static lv_obj_t *row(lv_obj_t *p, int gap)
{
    lv_obj_t *r = lv_obj_create(p);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, gap, 0);
    lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);
    return r;
}

static lv_obj_t *icon_btn(lv_obj_t *p, const char *sym, lv_event_cb_t cb)
{
    lv_obj_t *b = lv_button_create(p);
    lv_obj_set_size(b, 64, 56);
    lv_obj_set_style_bg_color(b, UI_C_SURFACE2, 0);
    lv_obj_set_style_radius(b, 16, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lbl(b, sym, &lv_font_montserrat_24, UI_C_TEXT);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static lv_obj_t *qr(lv_obj_t *p, const char *url, int size, const char *caption)
{
    lv_obj_t *c = col(p, 8);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
#if LV_USE_QRCODE
    lv_obj_t *q = lv_qrcode_create(c);
    lv_qrcode_set_size(q, size);
    lv_qrcode_set_dark_color(q, lv_color_black());
    lv_qrcode_set_light_color(q, lv_color_white());
    lv_qrcode_update(q, url, strlen(url));
    lv_obj_set_style_border_color(q, lv_color_white(), 0);
    lv_obj_set_style_border_width(q, 8, 0);
#else
    lbl(c, url, &lv_font_montserrat_14, UI_C_MUTED);
#endif
    lv_obj_t *cl = lbl(c, caption, &lv_font_montserrat_16, UI_C_TEXT);
    lv_obj_set_style_text_align(cl, LV_TEXT_ALIGN_CENTER, 0);
    return c;
}

/* ================================================================== actions */

static void refresh_cb(lv_event_t *e)
{
    (void)e;
    plat_weather_refresh(true);
    ui_toast("Updating...");
}

static void settings_cb(lv_event_t *e) { (void)e; go(V_SETTINGS); }
static void done_cb(lv_event_t *e) { (void)e; go(V_MAIN); }

static void key_done(const char *text, void *user)
{
    (void)user;
    char k[48];
    int n = 0;
    for (const char *p = text; *p && n < (int)sizeof(k) - 1; p++)
        if (*p != ' ' && *p != '\t') k[n++] = *p;
    k[n] = 0;
    snprintf(g_settings.wx_key, sizeof(g_settings.wx_key), "%s", k);
    ui_settings_changed();
    plat_weather_key_changed();
    ui_toast(k[0] ? "Key saved - checking it with OpenWeather" : "Key removed");
    go(s_view);
}

static void key_cb(lv_event_t *e)
{
    (void)e;
    ui_text_prompt("OpenWeather API key (32 characters)", "", false, 47, key_done, NULL);
}

/* ---- locations ---- */

static wx_loc_t s_found[8];
static int s_found_n;
static uint32_t s_search_gen;

static void found_pick(int i, void *user)
{
    (void)user;
    if (i < 0 || i >= s_found_n) return;
    if (g_settings.wx_loc_count >= WX_LOC_MAX) { ui_toast("The list is full (8 places)"); return; }
    int n = g_settings.wx_loc_count++;
    g_settings.wx_locs[n] = s_found[i];
    g_settings.wx_loc_sel = (uint8_t)n;
    ui_settings_changed();
    plat_weather_refresh(true);
    go(V_MAIN);
}

static void search_done(const wx_loc_t *res, int n, const char *err, void *user)
{
    if ((uint32_t)(uintptr_t)user != s_search_gen || !s_root) return;
    if (n <= 0) { ui_toast(err ? err : "No place found"); return; }
    s_found_n = n > 8 ? 8 : n;
    memcpy(s_found, res, sizeof(wx_loc_t) * s_found_n);
    static char items[8][100];
    static const char *ptrs[8];
    for (int i = 0; i < s_found_n; i++) {
        const wx_loc_t *l = &s_found[i];
        char t[100];
        snprintf(t, sizeof(t), "%s%s%s, %s", l->name, l->region[0] ? ", " : "", l->region, l->country);
        ascii_fold(items[i], sizeof(items[i]), t);
        ptrs[i] = items[i];
    }
    ui_menu("Choose the place", ptrs, s_found_n, found_pick, NULL);
}

static void search_text(const char *q, void *user)
{
    (void)user;
    if (!q[0]) return;
    plat_weather_search(q, search_done, (void *)(uintptr_t)++s_search_gen);
    ui_toast("Searching...");
}

static void add_loc_cb(lv_event_t *e)
{
    (void)e;
    if (!g_settings.wx_key[0]) { ui_toast("Enter your API key first"); return; }
    ui_text_prompt("Town or city (\"Barrie, ON\"), or postal / ZIP code", "", false, 60, search_text, NULL);
}

static void loc_select(int i)
{
    if (i < 0 || i >= g_settings.wx_loc_count) return;
    g_settings.wx_loc_sel = (uint8_t)i;
    ui_settings_changed();
    plat_weather_refresh(false);
    s_gen_seen = 0;
}

static void loc_menu_pick(int i, void *user)
{
    (void)user;
    if (i == g_settings.wx_loc_count) { add_loc_cb(NULL); return; }
    loc_select(i);
    go(V_MAIN);
}

static void place_cb(lv_event_t *e)
{
    (void)e;
    static char items[WX_LOC_MAX + 1][80];
    static const char *ptrs[WX_LOC_MAX + 1];
    int n = g_settings.wx_loc_count;
    for (int i = 0; i < n; i++) {
        char nm[48];
        ascii_fold(nm, sizeof(nm), g_settings.wx_locs[i].name);
        snprintf(items[i], sizeof(items[i]), "%s%s", i == g_settings.wx_loc_sel ? LV_SYMBOL_OK "  " : "", nm);
        ptrs[i] = items[i];
    }
    snprintf(items[n], sizeof(items[n]), LV_SYMBOL_PLUS "  Add a place");
    ptrs[n] = items[n];
    ui_menu("Places", ptrs, n + 1, loc_menu_pick, NULL);
}

static void step_loc(int d)
{
    int n = g_settings.wx_loc_count;
    if (n < 2) return;
    loc_select((g_settings.wx_loc_sel + d + n) % n);
    go(V_MAIN);
}

static void prev_cb(lv_event_t *e) { (void)e; step_loc(-1); }
static void next_cb(lv_event_t *e) { (void)e; step_loc(1); }

static void loc_row_cb(lv_event_t *e)
{
    loc_select((int)(intptr_t)lv_event_get_user_data(e));
    go(V_SETTINGS);
}

static int s_del_idx;
static void loc_del_ok(void *user)
{
    (void)user;
    int i = s_del_idx;
    if (i < 0 || i >= g_settings.wx_loc_count) return;
    memmove(&g_settings.wx_locs[i], &g_settings.wx_locs[i + 1], sizeof(wx_loc_t) * (g_settings.wx_loc_count - i - 1));
    g_settings.wx_loc_count--;
    memset(&g_settings.wx_locs[g_settings.wx_loc_count], 0, sizeof(wx_loc_t));
    if (g_settings.wx_loc_sel >= g_settings.wx_loc_count) g_settings.wx_loc_sel = g_settings.wx_loc_count ? g_settings.wx_loc_count - 1 : 0;
    else if (g_settings.wx_loc_sel > i) g_settings.wx_loc_sel--;
    ui_settings_changed();
    plat_weather_refresh(false);
    go(V_SETTINGS);
}

static void loc_del_cb(lv_event_t *e)
{
    s_del_idx = (int)(intptr_t)lv_event_get_user_data(e);
    char m[96];
    snprintf(m, sizeof(m), "Remove %s from the list?", g_settings.wx_locs[s_del_idx].name);
    ui_confirm("Remove place", m, "Remove", UI_C_ERR, loc_del_ok, NULL);
}

static void units_cb(lv_event_t *e)
{
    g_settings.wx_units = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    ui_settings_changed();
    go(V_SETTINGS);
}

static void aq_cb(lv_event_t *e)
{
    g_settings.wx_aq_scale = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    ui_settings_changed();
    go(V_SETTINGS);
}

static void home_cb(lv_event_t *e)
{
    g_settings.wx_home = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ui_settings_changed();
}

static void alert_cb(lv_event_t *e)
{
    (void)e;
    if (!s_w || !s_w->alert_event[0]) return;
    static char msg[560];
    char until[24];
    fmt_clock(until, sizeof(until), s_w->alert_end);
    snprintf(msg, sizeof(msg), "%s%s%s\n\n%s", s_w->alert_sender, s_w->alert_end ? " - until " : "", s_w->alert_end ? until : "",
             s_w->alert_desc[0] ? s_w->alert_desc : "No details were given.");
    ui_confirm(s_w->alert_event, msg, "OK", UI_C_ACCENT, NULL, NULL);
}

/* ================================================================== views */

static lv_obj_t *page(void)
{
    lv_obj_clean(s_root);
    s_refresh_btn = NULL;
    lv_obj_t *p = lv_obj_create(s_root);
    lv_obj_remove_style_all(p);
    lv_obj_set_size(p, lv_pct(100), lv_pct(100));
    lv_obj_set_style_pad_all(p, 16, 0);
    lv_obj_set_style_pad_row(p, 14, 0);
    lv_obj_set_flex_flow(p, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(p, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(p, LV_SCROLLBAR_MODE_ACTIVE);
    return p;
}

/* ---- setup (no key) ---- */

static void build_setup(lv_obj_t *p)
{
    lv_obj_t *c = ui_card(p);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *hr = row(c, 16);
    lv_obj_t *ic = lv_image_create(hr);
    lv_image_set_src(ic, &wx_02d_s);
    lbl(hr, "Set up Weather", ui_font(UI_FW_MEDIUM, 34), UI_C_TEXT);
    lv_obj_t *t = lbl(c,
                      "Weather comes from OpenWeather and needs your own free API key:\n"
                      "1.  Scan the first code and create a free account.\n"
                      "2.  Scan the second code to copy your API key.\n"
                      "3.  Tap Enter API key, or paste it on the web page (http://p4box.local).\n"
                      "New keys can take up to 2 hours before OpenWeather accepts them.",
                      &lv_font_montserrat_20, UI_C_MUTED);
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(t, 6, 0);
    lv_obj_t *qr_row = row(c, 40);
    lv_obj_set_flex_align(qr_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    qr(qr_row, URL_SIGNUP, 190, "1. Free sign-up");
    qr(qr_row, URL_KEYS, 190, "2. Your API keys");
    lv_obj_t *br = row(c, 14);
    lv_obj_set_flex_align(br, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_t *b = ui_button(br, LV_SYMBOL_KEYBOARD "  Enter API key", UI_C_ACCENT);
    lv_obj_add_event_cb(b, key_cb, LV_EVENT_CLICKED, NULL);
    b = ui_button(br, LV_SYMBOL_SETTINGS "  Settings", UI_C_SURFACE2);
    lv_obj_add_event_cb(b, settings_cb, LV_EVENT_CLICKED, NULL);
}

/* ---- main ---- */

static lv_obj_t *tile(lv_obj_t *grid, const char *title, const char *value, const char *sub, lv_color_t vcol)
{
    lv_obj_t *t = lv_obj_create(grid);
    lv_obj_set_size(t, 220, 116);
    lv_obj_set_style_bg_color(t, UI_C_SURFACE, 0);
    lv_obj_set_style_border_width(t, 1, 0);
    lv_obj_set_style_border_color(t, UI_C_BORDER, 0);
    lv_obj_set_style_radius(t, 16, 0);
    lv_obj_set_style_pad_all(t, 12, 0);
    lv_obj_set_style_pad_row(t, 0, 0);
    lv_obj_set_flex_flow(t, LV_FLEX_FLOW_COLUMN);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(t, LV_OBJ_FLAG_CLICKABLE);
    lbl(t, title, &lv_font_montserrat_16, UI_C_MUTED);
    lv_obj_t *v = lbl(t, value, ui_font(UI_FW_MEDIUM, 32), vcol);
    lv_obj_set_width(v, 196);
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_t *s = lbl(t, sub, &lv_font_montserrat_16, UI_C_MUTED);
    lv_obj_set_width(s, 196);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    return t;
}

static void build_header(lv_obj_t *p, const weather_t *w)
{
    lv_obj_t *h = row(p, 10);
    int n = g_settings.wx_loc_count;
    if (n > 1) icon_btn(h, LV_SYMBOL_LEFT, prev_cb);
    lv_obj_t *pl = lv_button_create(h);
    lv_obj_set_flex_grow(pl, 1);
    lv_obj_set_height(pl, 64);
    lv_obj_set_style_bg_opa(pl, LV_OPA_TRANSP, 0);
    lv_obj_set_style_bg_opa(pl, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_bg_color(pl, UI_C_SURFACE2, LV_STATE_PRESSED);
    lv_obj_set_style_shadow_width(pl, 0, 0);
    lv_obj_set_style_pad_all(pl, 4, 0);
    lv_obj_add_event_cb(pl, place_cb, LV_EVENT_CLICKED, NULL);
    const wx_loc_t *l = &g_settings.wx_locs[g_settings.wx_loc_sel];
    lv_obj_t *nm = lbl(pl, l->name, ui_font(UI_FW_MEDIUM, 30), UI_C_TEXT);
    lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
    lv_obj_set_width(nm, lv_pct(100));
    lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, -4);
    char sub[96];
    snprintf(sub, sizeof(sub), "%s%s%s   " LV_SYMBOL_DOWN, l->region, l->region[0] ? ", " : "", l->country);
    lv_obj_t *sl = lbl(pl, sub, ui_font(UI_FW_REGULAR, 18), UI_C_MUTED);
    lv_obj_align(sl, LV_ALIGN_BOTTOM_LEFT, 0, 2);
    if (n > 1) icon_btn(h, LV_SYMBOL_RIGHT, next_cb);
    s_refresh_btn = icon_btn(h, LV_SYMBOL_REFRESH, refresh_cb);
    if (plat_weather_busy()) lv_obj_set_style_text_color(lv_obj_get_child(s_refresh_btn, 0), UI_C_ACCENT, 0);
    icon_btn(h, LV_SYMBOL_SETTINGS, settings_cb);
    (void)w;
}

static void build_now(lv_obj_t *p, const weather_t *w)
{
    lv_obj_t *c = ui_card(p);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(c, 6, 0);
    lv_obj_set_style_pad_ver(c, 10, 0);
    lv_obj_t *ic = lv_image_create(c);
    lv_image_set_src(ic, wx_icon_img(w->icon, true));

    char t[16];
    fmt_temp(t, sizeof(t), w->temp);
    lv_obj_t *tl = lbl(c, t, ui_font(UI_FW_LIGHT, 104), UI_C_TEXT);
    lv_obj_set_style_text_letter_space(tl, -3, 0);
    lv_obj_set_style_pad_right(tl, 18, 0);

    lv_obj_t *r = col(c, 2);
    lv_obj_set_flex_grow(r, 1);
    lv_obj_t *d = lbl(r, w->desc[0] ? w->desc : "-", ui_font(UI_FW_MEDIUM, 28), UI_C_TEXT);
    lv_obj_set_width(d, 250);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    char fv[16], why[16], buf[96];
    feels_text(w, fv, sizeof(fv), why, sizeof(why));
    snprintf(buf, sizeof(buf), "%s %s", why, fv);
    lbl(r, buf, ui_font(UI_FW_REGULAR, 22), why[0] == 'F' ? UI_C_MUTED : UI_C_ACCENT2);
    if (w->n_days) {
        char hi[16], lo[16];
        fmt_temp(hi, sizeof(hi), w->days[0].tmax);
        fmt_temp(lo, sizeof(lo), w->days[0].tmin);
        snprintf(buf, sizeof(buf), "High %s   Low %s", hi, lo);
        lbl(r, buf, ui_font(UI_FW_REGULAR, 22), UI_C_TEXT);
    }
}

static void build_alert(lv_obj_t *p, const weather_t *w)
{
    if (!w->alert_event[0]) return;
    lv_obj_t *c = ui_card(p);
    lv_obj_set_style_border_color(c, UI_C_ACCENT2, 0);
    lv_obj_set_style_border_width(c, 2, 0);
    lv_obj_set_style_bg_color(c, lv_color_hex(0x2A1C10), 0);
    lv_obj_set_style_pad_row(c, 4, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(c, alert_cb, LV_EVENT_CLICKED, NULL);
    char b[128], until[24];
    snprintf(b, sizeof(b), LV_SYMBOL_WARNING "  %s", w->alert_event);
    lv_obj_t *t = lbl(c, b, ui_font(UI_FW_MEDIUM, 26), UI_C_ACCENT2);
    lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
    lv_obj_set_width(t, lv_pct(100));
    fmt_clock(until, sizeof(until), w->alert_end);
    snprintf(b, sizeof(b), "%s%s%s  -  tap for details", w->alert_sender, w->alert_end ? ", until " : "", w->alert_end ? until : "");
    lv_obj_t *s = lbl(c, b, &lv_font_montserrat_16, UI_C_MUTED);
    lv_label_set_long_mode(s, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s, lv_pct(100));
}

static void build_hours(lv_obj_t *p, const weather_t *w)
{
    if (!w->n_hours) return;
    char t[48];
    snprintf(t, sizeof(t), w->hour_step_h == 1 ? "NEXT HOURS" : "NEXT 24 HOURS (3-HOUR STEPS)");
    ui_section_title(p, t);
    lv_obj_t *c = ui_card(p);
    lv_obj_set_style_pad_all(c, 10, 0);
    lv_obj_set_flex_flow(c, LV_FLEX_FLOW_ROW);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(c, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(c, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_pad_column(c, 4, 0);
    lv_obj_add_flag(c, LV_OBJ_FLAG_SCROLL_CHAIN_VER);
    time_t now = plat_now();
    int shown = 0;
    for (int i = 0; i < w->n_hours; i++) {
        const wx_hour_t *h = &w->hours[i];
        if (h->dt + 3600 * w->hour_step_h <= now) continue;   /* already past */
        lv_obj_t *k = col(c, 2);
        lv_obj_set_width(k, 82);
        lv_obj_set_flex_align(k, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        char b[16];
        if (shown == 0 && h->dt <= now + 1800) snprintf(b, sizeof(b), "Now");
        else fmt_hour_short(b, sizeof(b), h->dt);
        lbl(k, b, &lv_font_montserrat_16, UI_C_MUTED);
        lv_obj_t *im = lv_image_create(k);
        lv_image_set_src(im, wx_icon_img(h->icon, false));
        fmt_temp(b, sizeof(b), h->temp);
        lbl(k, b, ui_font(UI_FW_MEDIUM, 24), UI_C_TEXT);
        if (h->pop >= 0.1f) snprintf(b, sizeof(b), "%d%%", (int)lroundf(h->pop * 100));
        else b[0] = 0;
        lbl(k, b[0] ? b : " ", &lv_font_montserrat_16, lv_color_hex(0x56AAFF));
        shown++;
    }
}

static void build_tiles(lv_obj_t *p, const weather_t *w)
{
    lv_obj_t *g = lv_obj_create(p);
    lv_obj_remove_style_all(g);
    lv_obj_set_size(g, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(g, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(g, 12, 0);
    lv_obj_set_style_pad_row(g, 12, 0);
    lv_obj_remove_flag(g, LV_OBJ_FLAG_SCROLLABLE);
    char v[32], s[64], a[24];

    /* precipitation: chance today + amount */
    if (w->n_days) {
        snprintf(v, sizeof(v), "%d%%", (int)lroundf(w->days[0].pop * 100));
        float mm = w->days[0].rain_mm + w->days[0].snow_mm;
        if (w->rain_1h + w->snow_1h > 0) { fmt_precip(a, sizeof(a), w->rain_1h + w->snow_1h); snprintf(s, sizeof(s), "%s last hour", a); }
        else if (mm > 0) { fmt_precip(a, sizeof(a), mm); snprintf(s, sizeof(s), "%s%s expected", a, w->days[0].snow_mm > w->days[0].rain_mm ? " snow" : ""); }
        else snprintf(s, sizeof(s), "Chance today");
    } else {
        snprintf(v, sizeof(v), "-");
        snprintf(s, sizeof(s), " ");
    }
    tile(g, "Precipitation", v, s, UI_C_TEXT);

    char fv[16], why[16];
    feels_text(w, fv, sizeof(fv), why, sizeof(why));
    fmt_temp(a, sizeof(a), w->feels);
    bool plain = !strcmp(why, "Feels like");
    snprintf(s, sizeof(s), "%s", plain ? "Wind and humidity" : !strcmp(why, "Wind chill") ? "Wind on bare skin" : "Heat and humidity");
    tile(g, why, fv, s, plain ? UI_C_TEXT : UI_C_ACCENT2);

    snprintf(v, sizeof(v), w->humidity == WX_NA ? "-" : "%.0f%%", w->humidity);
    fmt_temp(a, sizeof(a), w->dew_point);
    snprintf(s, sizeof(s), "Dew point %s", a);
    tile(g, "Humidity", v, s, UI_C_TEXT);

    fmt_wind(v, sizeof(v), w->wind_ms);
    if (w->gust_ms != WX_NA && w->gust_ms > w->wind_ms) { fmt_wind(a, sizeof(a), w->gust_ms); snprintf(s, sizeof(s), "%s, gusts %s", wx_compass(w->wind_deg), a); }
    else snprintf(s, sizeof(s), "From the %s", wx_compass(w->wind_deg));
    tile(g, "Wind", v, s, UI_C_TEXT);

    float uv = w->uvi;
    if (uv == WX_NA && w->n_days && w->days[0].uvi != WX_NA) uv = w->days[0].uvi;
    if (uv != WX_NA) {
        snprintf(v, sizeof(v), "%.0f", uv);
        if (w->n_days && w->days[0].uvi != WX_NA && w->days[0].uvi > uv + 0.5f) snprintf(s, sizeof(s), "%s, today max %.0f", wx_uv_label(uv), w->days[0].uvi);
        else snprintf(s, sizeof(s), "%s", wx_uv_label(uv));
        lv_color_t c = uv < 3 ? UI_C_OK : uv < 6 ? UI_C_WARN : uv < 8 ? lv_color_hex(0xF0883E) : UI_C_ERR;
        tile(g, "UV index", v, s, c);
    } else {
        tile(g, "UV index", "-", "Needs One Call", UI_C_MUTED);
    }

    lv_color_t ac;
    aq_text(w, v, sizeof(v), s, sizeof(s), &ac);
    tile(g, "Air quality", v, s, ac);

    if (w->pressure == WX_NA) snprintf(v, sizeof(v), "-");
    else if (imperial()) snprintf(v, sizeof(v), "%.2f inHg", w->pressure * 0.02953f);
    else snprintf(v, sizeof(v), "%.1f kPa", w->pressure / 10.0f);
    tile(g, "Pressure", v, "At sea level", UI_C_TEXT);

    if (w->visibility_m == WX_NA) snprintf(v, sizeof(v), "-");
    else if (imperial()) snprintf(v, sizeof(v), "%.1f mi", w->visibility_m / 1609.34f);
    else snprintf(v, sizeof(v), w->visibility_m >= 10000 ? "%.0f+ km" : "%.1f km", w->visibility_m / 1000.0f);
    snprintf(s, sizeof(s), w->clouds == WX_NA ? " " : "Cloud cover %.0f%%", w->clouds);
    tile(g, "Visibility", v, s, UI_C_TEXT);

    fmt_clock(v, sizeof(v), w->sunrise);
    long day = (w->sunrise && w->sunset > w->sunrise) ? (long)(w->sunset - w->sunrise) : 0;
    if (day) snprintf(s, sizeof(s), "Daylight %ld h %02ld min", day / 3600, (day % 3600) / 60);
    else snprintf(s, sizeof(s), " ");
    tile(g, "Sunrise", v, s, UI_C_TEXT);

    fmt_clock(v, sizeof(v), w->sunset);
    tile(g, "Sunset", v, " ", UI_C_TEXT);

    if (w->n_days && w->days[0].moon_phase != WX_NA) {
        tile(g, "Moon", wx_moon_name(w->days[0].moon_phase), " ", UI_C_TEXT);
    } else if (w->aq_valid) {
        snprintf(v, sizeof(v), "%.0f", w->pm2_5);
        snprintf(s, sizeof(s), "PM10 %.0f, ozone %.0f ug/m3", w->pm10, w->o3);
        tile(g, "Fine particles PM2.5", v, s, UI_C_TEXT);
    }
}

static void build_days(lv_obj_t *p, const weather_t *w)
{
    if (!w->n_days) return;
    char t[40];
    snprintf(t, sizeof(t), "%d-DAY FORECAST", w->n_days);
    ui_section_title(p, t);
    lv_obj_t *c = ui_card(p);
    lv_obj_set_style_pad_ver(c, 6, 0);
    lv_obj_set_style_pad_row(c, 0, 0);
    float lo = 999, hi = -999;
    for (int i = 0; i < w->n_days; i++) {
        if (w->days[i].tmin != WX_NA && w->days[i].tmin < lo) lo = w->days[i].tmin;
        if (w->days[i].tmax != WX_NA && w->days[i].tmax > hi) hi = w->days[i].tmax;
    }
    if (hi - lo < 1) hi = lo + 1;
    struct tm today;
    loc_tm(plat_now(), &today);
    for (int i = 0; i < w->n_days; i++) {
        const wx_day_t *d = &w->days[i];
        lv_obj_t *r = row(c, 10);
        lv_obj_set_height(r, 70);
        if (i) {
            lv_obj_set_style_border_side(r, LV_BORDER_SIDE_TOP, 0);
            lv_obj_set_style_border_width(r, 1, 0);
            lv_obj_set_style_border_color(r, UI_C_SURFACE2, 0);
        }
        struct tm dt;
        loc_tm(d->dt, &dt);
        char b[24];
        if (dt.tm_yday == today.tm_yday && dt.tm_year == today.tm_year) snprintf(b, sizeof(b), "Today");
        else strftime(b, sizeof(b), "%a %e", &dt);
        lv_obj_t *dl = lbl(r, b, ui_font(UI_FW_MEDIUM, 24), UI_C_TEXT);
        lv_obj_set_width(dl, 110);
        lv_obj_t *im = lv_image_create(r);
        lv_image_set_src(im, wx_icon_img(d->icon, false));
        if (d->pop >= 0.1f) snprintf(b, sizeof(b), "%d%%", (int)lroundf(d->pop * 100));
        else b[0] = 0;
        lv_obj_t *pl = lbl(r, b, &lv_font_montserrat_20, lv_color_hex(0x56AAFF));
        lv_obj_set_width(pl, 60);
        fmt_temp(b, sizeof(b), d->tmin);
        lv_obj_t *l1 = lbl(r, b, ui_font(UI_FW_REGULAR, 24), UI_C_MUTED);
        lv_obj_set_width(l1, 62);
        lv_obj_set_style_text_align(l1, LV_TEXT_ALIGN_RIGHT, 0);
        /* range bar */
        lv_obj_t *bar = lv_obj_create(r);
        lv_obj_remove_style_all(bar);
        lv_obj_set_size(bar, 1, 8);
        lv_obj_set_flex_grow(bar, 1);
        lv_obj_set_style_bg_color(bar, UI_C_SURFACE2, 0);
        lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
        lv_obj_set_style_radius(bar, 4, 0);
        lv_obj_update_layout(r);
        int bw = lv_obj_get_width(bar);
        if (bw < 40) bw = 160;
        lv_obj_t *seg = lv_obj_create(bar);
        lv_obj_remove_style_all(seg);
        float a0 = d->tmin == WX_NA ? lo : d->tmin, a1 = d->tmax == WX_NA ? hi : d->tmax;
        int x0 = (int)((a0 - lo) / (hi - lo) * bw), x1 = (int)((a1 - lo) / (hi - lo) * bw);
        if (x1 - x0 < 8) x1 = x0 + 8;
        lv_obj_set_pos(seg, x0, 0);
        lv_obj_set_size(seg, x1 - x0, 8);
        lv_obj_set_style_radius(seg, 4, 0);
        lv_obj_set_style_bg_opa(seg, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(seg, lv_color_hex(0x56AAFF), 0);
        lv_obj_set_style_bg_grad_color(seg, UI_C_ACCENT2, 0);
        lv_obj_set_style_bg_grad_dir(seg, LV_GRAD_DIR_HOR, 0);
        fmt_temp(b, sizeof(b), d->tmax);
        lv_obj_t *l2 = lbl(r, b, ui_font(UI_FW_MEDIUM, 24), UI_C_TEXT);
        lv_obj_set_width(l2, 62);
    }
}

static void build_footer(lv_obj_t *p, const weather_t *w)
{
    char b[200], t[24] = "-";
    if (w->fetched) {
        time_t f = w->fetched;
        struct tm tm;
        localtime_r(&f, &tm);
        ui_fmt_time(t, sizeof(t), &tm, false, true);
    }
    const char *src = w->source == WX_SRC_ONECALL4 ? "One Call 4.0" : w->source == WX_SRC_ONECALL3 ? "One Call 3.0" : "free plan";
    snprintf(b, sizeof(b), "Updated %s  -  OpenWeather, %s", t, src);
    lv_obj_t *l = lbl(p, b, &lv_font_montserrat_16, UI_C_MUTED);
    lv_obj_set_width(l, lv_pct(100));
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
}

static void build_main(lv_obj_t *p)
{
    if (!g_settings.wx_key[0]) { build_setup(p); return; }
    if (!g_settings.wx_loc_count) {
        lv_obj_t *c = ui_card(p);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *ic = lv_image_create(c);
        lv_image_set_src(ic, &wx_02d_l);
        lbl(c, "Add your first place", ui_font(UI_FW_MEDIUM, 34), UI_C_TEXT);
        lv_obj_t *t = lbl(c, "Search by town or city (for example \"Wasaga Beach, ON\"),\nor by postal or ZIP code.", &lv_font_montserrat_20, UI_C_MUTED);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_t *br = row(c, 14);
        lv_obj_set_flex_align(br, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        lv_obj_t *b = ui_button(br, LV_SYMBOL_PLUS "  Add a place", UI_C_ACCENT);
        lv_obj_add_event_cb(b, add_loc_cb, LV_EVENT_CLICKED, NULL);
        b = ui_button(br, LV_SYMBOL_SETTINGS "  Settings", UI_C_SURFACE2);
        lv_obj_add_event_cb(b, settings_cb, LV_EVENT_CLICKED, NULL);
        return;
    }
    plat_weather_get(&g_settings.wx_locs[g_settings.wx_loc_sel], s_w);
    build_header(p, s_w);
    if (!s_w->valid || s_w->temp == WX_NA) {
        lv_obj_t *c = ui_card(p);
        lv_obj_set_flex_align(c, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
        bool err = s_w->valid && s_w->error[0];
        lv_obj_t *t = lbl(c, err ? s_w->error : "Getting the weather...", ui_font(UI_FW_REGULAR, 26), err ? UI_C_WARN : UI_C_MUTED);
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
        lv_obj_set_style_text_align(t, LV_TEXT_ALIGN_CENTER, 0);
        if (err) {
            lv_obj_t *b = ui_button(c, LV_SYMBOL_SETTINGS "  Check the API key", UI_C_SURFACE2);
            lv_obj_add_event_cb(b, settings_cb, LV_EVENT_CLICKED, NULL);
        }
        return;
    }
    build_now(p, s_w);
    build_alert(p, s_w);
    build_hours(p, s_w);
    build_tiles(p, s_w);
    build_days(p, s_w);
    if (s_w->error[0]) {
        char b[140];
        snprintf(b, sizeof(b), LV_SYMBOL_WARNING "  Last update failed: %s", s_w->error);
        lv_obj_t *l = lbl(p, b, &lv_font_montserrat_16, UI_C_WARN);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    build_footer(p, s_w);
}

/* ---- settings ---- */

static lv_obj_t *pill(lv_obj_t *parent, const char *text, bool on, lv_event_cb_t cb, int v, int w)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, w, 52);
    lv_obj_set_style_radius(b, 26, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_set_style_bg_color(b, on ? UI_C_ACCENT : UI_C_SURFACE2, 0);
    lv_obj_t *l = lbl(b, text, &lv_font_montserrat_20, on ? lv_color_white() : UI_C_MUTED);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, (void *)(intptr_t)v);
    return b;
}

static void build_settings(lv_obj_t *p)
{
    lv_obj_t *h = lv_obj_create(p);
    lv_obj_remove_style_all(h);
    lv_obj_set_size(h, lv_pct(100), 60);
    lbl(h, "Weather settings", ui_font(UI_FW_MEDIUM, 32), UI_C_TEXT);
    lv_obj_t *db = ui_button(h, "Done", UI_C_ACCENT);
    lv_obj_set_width(db, 120);
    lv_obj_align(db, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(db, done_cb, LV_EVENT_CLICKED, NULL);

    /* places */
    ui_section_title(p, "PLACES");
    lv_obj_t *c = ui_card(p);
    lv_obj_set_style_pad_row(c, 8, 0);
    for (int i = 0; i < g_settings.wx_loc_count; i++) {
        const wx_loc_t *l = &g_settings.wx_locs[i];
        lv_obj_t *r = row(c, 10);
        bool sel = i == g_settings.wx_loc_sel;
        lv_obj_t *b = lv_button_create(r);
        lv_obj_set_flex_grow(b, 1);
        lv_obj_set_height(b, 60);
        lv_obj_set_style_bg_color(b, sel ? lv_color_hex(0x1B2B45) : UI_C_SURFACE2, 0);
        lv_obj_set_style_border_width(b, sel ? 2 : 0, 0);
        lv_obj_set_style_border_color(b, UI_C_ACCENT, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_add_event_cb(b, loc_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        char t[110];
        snprintf(t, sizeof(t), "%s%s  %s%s%s", sel ? LV_SYMBOL_OK "  " : "", l->name, l->region, l->region[0] ? ", " : "", l->country);
        lv_obj_t *tl = lbl(b, t, ui_font(UI_FW_REGULAR, 22), UI_C_TEXT);
        lv_label_set_long_mode(tl, LV_LABEL_LONG_DOT);
        lv_obj_set_width(tl, lv_pct(100));
        lv_obj_align(tl, LV_ALIGN_LEFT_MID, 0, 0);
        lv_obj_t *del = ui_button(r, LV_SYMBOL_TRASH, UI_C_SURFACE2);
        lv_obj_set_style_text_color(del, UI_C_ERR, 0);
        lv_obj_add_event_cb(del, loc_del_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
    if (g_settings.wx_loc_count < WX_LOC_MAX) {
        lv_obj_t *ab = ui_button(c, LV_SYMBOL_PLUS "  Add a place", UI_C_SURFACE2);
        lv_obj_set_width(ab, lv_pct(100));
        lv_obj_add_event_cb(ab, add_loc_cb, LV_EVENT_CLICKED, NULL);
    }

    /* display */
    ui_section_title(p, "DISPLAY");
    c = ui_card(p);
    lv_obj_t *r = ui_row(c, "Units");
    lv_obj_t *pr = row(r, 10);
    lv_obj_set_width(pr, LV_SIZE_CONTENT);
    pill(pr, "Metric (\xC2\xB0" "C, km/h)", !imperial(), units_cb, WX_METRIC, 210);
    pill(pr, "Imperial (\xC2\xB0" "F, mph)", imperial(), units_cb, WX_IMPERIAL, 210);
    lbl(c, "Air quality scale", &lv_font_montserrat_20, UI_C_TEXT);
    pr = row(c, 10);
    pill(pr, "AQHI (Canada)", g_settings.wx_aq_scale == WX_AQ_AQHI, aq_cb, WX_AQ_AQHI, 204);
    pill(pr, "US AQI", g_settings.wx_aq_scale == WX_AQ_US, aq_cb, WX_AQ_US, 180);
    pill(pr, "OpenWeather 1-5", g_settings.wx_aq_scale == WX_AQ_OWM, aq_cb, WX_AQ_OWM, 236);
    r = ui_row(c, "Show the weather on the home screen");
    lv_obj_t *sw = lv_switch_create(r);
    lv_obj_set_size(sw, 84, 44);
    lv_obj_set_state(sw, LV_STATE_CHECKED, g_settings.wx_home);
    lv_obj_add_event_cb(sw, home_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *n = lbl(c, imperial() ? "Imperial: \"feels like\" uses the US heat index in summer and wind chill in winter."
                                    : "Metric: humidex in summer and wind chill in winter, as Environment Canada reports them.",
                      &lv_font_montserrat_16, UI_C_MUTED);
    lv_obj_set_width(n, lv_pct(100));
    lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);

    /* key */
    ui_section_title(p, "OPENWEATHER API KEY");
    c = ui_card(p);
    r = ui_row(c, "API key");
    char k[48];
    size_t kl = strlen(g_settings.wx_key);
    if (kl) snprintf(k, sizeof(k), "....%s", g_settings.wx_key + (kl > 4 ? kl - 4 : 0));
    else snprintf(k, sizeof(k), "Not set");
    lbl(r, k, &lv_font_montserrat_20, kl ? UI_C_TEXT : UI_C_WARN);
    lv_obj_t *kb = ui_button(r, kl ? "Change" : "Enter key", UI_C_ACCENT);
    lv_obj_add_event_cb(kb, key_cb, LV_EVENT_CLICKED, NULL);
    char st[200];
    weather_t *w = s_w;
    if (g_settings.wx_loc_count) plat_weather_get(&g_settings.wx_locs[g_settings.wx_loc_sel], w);
    else memset(w, 0, sizeof(*w));
    if (!kl) snprintf(st, sizeof(st), "Enter a key to get the weather.");
    else if (w->valid && w->error[0]) snprintf(st, sizeof(st), LV_SYMBOL_WARNING "  %s", w->error);
    else if (w->valid) snprintf(st, sizeof(st), LV_SYMBOL_OK "  Key works. One Call: %s.", plat_weather_onecall());
    else snprintf(st, sizeof(st), "Not checked yet (add a place first).");
    lv_obj_t *sl = lbl(c, st, &lv_font_montserrat_20, w->valid && w->error[0] ? UI_C_WARN : UI_C_MUTED);
    lv_obj_set_width(sl, lv_pct(100));
    lv_label_set_long_mode(sl, LV_LABEL_LONG_WRAP);
    lv_obj_t *info = lbl(c,
                         "Free key: current weather, 5-day forecast, air quality and place search.\n"
                         "Add the free One Call subscription (first 1,000 calls a day free) for the UV index, "
                         "hourly and 8-day forecasts, the moon and weather alerts. The panel uses about 300 a day.\n"
                         "Tip: the key is easier to paste on the web page, http://p4box.local",
                         &lv_font_montserrat_16, UI_C_MUTED);
    lv_obj_set_width(info, lv_pct(100));
    lv_label_set_long_mode(info, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_line_space(info, 4, 0);
    lv_obj_t *qr_row = row(c, 26);
    lv_obj_set_flex_align(qr_row, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    qr(qr_row, URL_SIGNUP, 160, "Free sign-up");
    qr(qr_row, URL_KEYS, 160, "Your API keys");
    qr(qr_row, URL_ONECALL, 160, "One Call (optional)");
}

/* ================================================================== app */

static void show(int view)
{
    if (!s_root) return;
    s_view = view;
    lv_obj_t *p = page();
    if (view == V_SETTINGS) build_settings(p);
    else build_main(p);
    s_gen_seen = plat_weather_gen();
    s_busy_seen = plat_weather_busy();
}

static void show_async(void *arg)
{
    int view = (int)(intptr_t)arg;
    if (!s_root) return;
    lv_obj_t *p = lv_obj_get_child(s_root, 0);
    int32_t y = p && view == s_view ? lv_obj_get_scroll_y(p) : 0;   /* keep the scroll position on a refresh */
    show(view);
    p = lv_obj_get_child(s_root, 0);
    if (p && y) {
        lv_obj_update_layout(p);
        lv_obj_scroll_to_y(p, y, LV_ANIM_OFF);
    }
}

static void go(int view) { lv_async_call(show_async, (void *)(intptr_t)view); }

static void timer_cb(lv_timer_t *t)
{
    (void)t;
    if (s_view != V_MAIN) return;
    bool busy = plat_weather_busy();
    if (plat_weather_gen() != s_gen_seen) {
        if (busy != s_busy_seen && s_refresh_btn && busy) {
            /* just the spinner colour */
            lv_obj_set_style_text_color(lv_obj_get_child(s_refresh_btn, 0), UI_C_ACCENT, 0);
            s_busy_seen = busy;
            s_gen_seen = plat_weather_gen();
            return;
        }
        lv_async_call(show_async, (void *)(intptr_t)V_MAIN);
        s_gen_seen = plat_weather_gen();
    }
}

static void weather_create(lv_obj_t *parent)
{
    s_root = parent;
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    if (!s_w) s_w = calloc(1, sizeof(weather_t));
    s_view = V_MAIN;
    show(V_MAIN);
    s_timer = lv_timer_create(timer_cb, 1000, NULL);
    plat_weather_refresh(false);
}

static void weather_destroy(void)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    s_root = NULL;
    s_refresh_btn = NULL;
}

static void weather_on_settings(void)
{
    if (s_root) lv_async_call(show_async, (void *)(intptr_t)s_view);
}

const ui_app_t app_weather = {
    .name = "Weather",
    .icon = &icon_weather,
    .create = weather_create,
    .destroy = weather_destroy,
    .on_settings = weather_on_settings,
};

/* ================================================================== home screen line */

/* Fills the small home-screen summary; false when there is nothing to show. */
bool wx_home_summary(const lv_image_dsc_t **icon, char *text, size_t n)
{
    if (!g_settings.wx_home || !g_settings.wx_key[0] || !g_settings.wx_loc_count) return false;
    static weather_t *w;
    static uint32_t gen = 0xFFFFFFFF;
    static int sel = -1;
    if (!w) w = calloc(1, sizeof(weather_t));
    if (!w) return false;
    if (gen != plat_weather_gen() || sel != g_settings.wx_loc_sel) {
        plat_weather_get(&g_settings.wx_locs[g_settings.wx_loc_sel], w);
        gen = plat_weather_gen();
        sel = g_settings.wx_loc_sel;
    }
    if (!w->valid || w->temp == WX_NA) return false;
    char t[16];
    float c = w->temp;
    snprintf(t, sizeof(t), "%d\xC2\xB0%s", (int)lroundf(imperial() ? c * 9 / 5 + 32 : c), imperial() ? "F" : "C");
    snprintf(text, n, "%s  %s", t, w->desc);
    *icon = wx_icon_img(w->icon, false);
    return true;
}
