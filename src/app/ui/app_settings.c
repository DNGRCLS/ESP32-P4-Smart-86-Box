/* Settings app: Network / Display / Time / System tabs. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_internal.h"
#include "../core/tz_table.h"
#include "../core/lockcore.h"

static lv_obj_t *s_tv;
static lv_timer_t *s_timer;
static uint32_t s_gen;            /* bumps each time the app is (re)created; guards async callbacks */

/* network tab */
static lv_obj_t *s_eth_val, *s_wifi_val, *s_web_val, *s_portal_btn_lbl, *s_scan_btn_lbl, *s_scan_list;
/* time tab */
static lv_obj_t *s_region_dd, *s_city_dd, *s_dst_dd, *s_tz_note, *s_now_lbl, *s_rtc_lbl;
static int s_city_map[128];       /* city dropdown index -> tz table index */
static int s_city_count;

/* ------------------------------------------------------------------ widget helpers */

static lv_obj_t *mk_switch(lv_obj_t *row, bool on, lv_event_cb_t cb)
{
    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, 84, 44);
    lv_obj_set_state(sw, LV_STATE_CHECKED, on);
    lv_obj_add_event_cb(sw, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

static lv_obj_t *mk_dropdown(lv_obj_t *row, const char *opts, int sel, int width, lv_event_cb_t cb)
{
    lv_obj_t *dd = lv_dropdown_create(row);
    lv_dropdown_set_options(dd, opts);
    lv_dropdown_set_selected(dd, sel);
    lv_obj_set_width(dd, width);
    lv_obj_set_style_text_font(dd, &lv_font_montserrat_20, 0);
    lv_obj_set_style_bg_color(dd, UI_C_SURFACE2, 0);
    lv_obj_set_style_border_color(dd, UI_C_BORDER, 0);
    lv_obj_t *list = lv_dropdown_get_list(dd);
    lv_obj_set_style_text_font(list, &lv_font_montserrat_20, 0);
    lv_obj_set_style_max_height(list, 420, 0);
    if (cb) lv_obj_add_event_cb(dd, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return dd;
}

static lv_obj_t *mk_slider(lv_obj_t *parent, int min, int max, int val, lv_event_cb_t cb)
{
    lv_obj_t *s = lv_slider_create(parent);
    lv_obj_set_width(s, lv_pct(100));
    lv_obj_set_height(s, 18);
    lv_slider_set_range(s, min, max);
    lv_slider_set_value(s, val, LV_ANIM_OFF);
    lv_obj_set_style_pad_ver(s, 12, 0);
    lv_obj_set_ext_click_area(s, 20);
    lv_obj_add_event_cb(s, cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s, cb, LV_EVENT_RELEASED, NULL);
    return s;
}

static lv_obj_t *tab_page(lv_obj_t *tab)
{
    lv_obj_set_flex_flow(tab, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(tab, 18, 0);
    lv_obj_set_style_pad_row(tab, 14, 0);
    lv_obj_set_style_bg_color(tab, UI_C_BG, 0);
    lv_obj_set_scroll_dir(tab, LV_DIR_VER);
    return tab;
}

static lv_obj_t *value_label(lv_obj_t *row)
{
    lv_obj_t *v = lv_label_create(row);
    lv_obj_set_style_text_color(v, UI_C_MUTED, 0);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_font(v, &lv_font_montserrat_20, 0);
    return v;
}

/* ================================================================== NETWORK */

static void wifi_connect_with_pass(const char *pass, void *user)
{
    char *ssid = (char *)user;
    plat_wifi_connect(ssid, pass);
    char msg[80];
    snprintf(msg, sizeof(msg), "Connecting to %s...", ssid);
    ui_toast(msg);
    free(ssid);
}

static void scan_item_cb(lv_event_t *e)
{
    lv_obj_t *btn = lv_event_get_current_target(e);
    const char *ssid = (const char *)lv_obj_get_user_data(btn);
    bool secure = lv_obj_has_flag(btn, LV_OBJ_FLAG_USER_1);
    char *copy = strdup(ssid);
    if (secure) {
        char title[64];
        snprintf(title, sizeof(title), "Password for %s", ssid);
        const char *prefill = strcmp(ssid, g_settings.wifi_ssid) == 0 ? g_settings.wifi_pass : "";
        ui_text_prompt(title, prefill, true, 64, wifi_connect_with_pass, copy);
    } else {
        wifi_connect_with_pass("", copy);
    }
}

static void scan_item_delete_cb(lv_event_t *e)
{
    free(lv_obj_get_user_data(lv_event_get_current_target(e)));
}

static void scan_done(const wifi_scan_rec_t *recs, int n, void *user)
{
    if ((uint32_t)(uintptr_t)user != s_gen || !s_scan_list) return;
    lv_label_set_text(s_scan_btn_lbl, LV_SYMBOL_REFRESH "  Scan again");
    lv_obj_clean(s_scan_list);
    if (n == 0) {
        lv_obj_t *l = lv_label_create(s_scan_list);
        lv_label_set_text(l, "No networks found.");
        lv_obj_set_style_text_color(l, UI_C_MUTED, 0);
        return;
    }
    for (int i = 0; i < n; i++) {
        if (!recs[i].ssid[0]) continue;
        lv_obj_t *b = lv_button_create(s_scan_list);
        lv_obj_set_size(b, lv_pct(100), 64);
        lv_obj_set_style_bg_color(b, UI_C_SURFACE2, 0);
        lv_obj_set_style_shadow_width(b, 0, 0);
        lv_obj_set_style_radius(b, 12, 0);
        lv_obj_set_user_data(b, strdup(recs[i].ssid));
        if (recs[i].secure) lv_obj_add_flag(b, LV_OBJ_FLAG_USER_1);
        lv_obj_add_event_cb(b, scan_item_cb, LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(b, scan_item_delete_cb, LV_EVENT_DELETE, NULL);

        lv_obj_t *n1 = lv_label_create(b);
        lv_label_set_text(n1, recs[i].ssid);
        lv_label_set_long_mode(n1, LV_LABEL_LONG_DOT);
        lv_obj_set_width(n1, 440);
        lv_obj_align(n1, LV_ALIGN_LEFT_MID, 4, 0);
        lv_obj_set_style_text_font(n1, &lv_font_montserrat_20, 0);

        int bars = recs[i].rssi > -55 ? 4 : recs[i].rssi > -65 ? 3 : recs[i].rssi > -75 ? 2 : 1;
        lv_obj_t *n2 = lv_label_create(b);
        lv_label_set_text_fmt(n2, "%d dBm  " LV_SYMBOL_WIFI, recs[i].rssi);
        lv_obj_set_style_text_color(n2, bars >= 3 ? UI_C_MUTED : UI_C_WARN, 0);
        lv_obj_align(n2, LV_ALIGN_RIGHT_MID, -4, 0);
        if (recs[i].secure) {
            lv_obj_t *pl = lv_image_create(b);
            lv_image_set_src(pl, &icon_padlock_small);
            lv_obj_set_style_image_recolor(pl, UI_C_MUTED, 0);
            lv_obj_set_style_image_recolor_opa(pl, LV_OPA_COVER, 0);
            lv_obj_align_to(pl, n2, LV_ALIGN_OUT_LEFT_MID, -14, 0);
        }
    }
}

static void scan_cb(lv_event_t *e)
{
    (void)e;
    lv_label_set_text(s_scan_btn_lbl, "Scanning...");
    lv_obj_clean(s_scan_list);
    plat_wifi_scan_async(scan_done, (void *)(uintptr_t)s_gen);
}

static void portal_cb(lv_event_t *e)
{
    (void)e;
    net_status_t st;
    plat_net_get_status(&st);
    if (st.portal_active) {
        plat_portal_stop();
        ui_toast("Setup hotspot stopped");
    } else {
        plat_portal_start();
        ui_toast("Setup hotspot started - join it from your phone");
    }
}

static void forget_ok(void *u)
{
    (void)u;
    plat_wifi_forget();
    ui_toast("Wi-Fi network forgotten");
}

static void forget_cb(lv_event_t *e)
{
    (void)e;
    if (!g_settings.wifi_ssid[0]) { ui_toast("No Wi-Fi network saved"); return; }
    char msg[128];
    snprintf(msg, sizeof(msg), "Remove the saved network \"%s\"? The panel stays online through Ethernet if a cable is connected.",
             g_settings.wifi_ssid);
    ui_confirm("Forget Wi-Fi", msg, "Forget", UI_C_ERR, forget_ok, NULL);
}

static void refresh_network(void)
{
    if (!s_eth_val) return;
    net_status_t st;
    plat_net_get_status(&st);
    char buf[96];
    if (st.eth_has_ip) snprintf(buf, sizeof(buf), "%s%s", st.eth_ip, st.active == NET_ACTIVE_ETH ? "  (active)" : "");
    else if (st.eth_link) snprintf(buf, sizeof(buf), "Cable connected, getting address...");
    else snprintf(buf, sizeof(buf), "No cable");
    lv_label_set_text(s_eth_val, buf);
    lv_obj_set_style_text_color(s_eth_val, st.eth_has_ip ? UI_C_OK : UI_C_MUTED, 0);

    if (st.wifi_connected)
        snprintf(buf, sizeof(buf), "%s  %d dBm\n%s%s", st.wifi_ssid, st.wifi_rssi, st.wifi_ip,
                 st.active == NET_ACTIVE_WIFI ? "  (active)" : "");
    else if (st.wifi_connecting) snprintf(buf, sizeof(buf), "Connecting to %s...", st.wifi_ssid);
    else if (st.wifi_configured) snprintf(buf, sizeof(buf), "%s - not connected", st.wifi_ssid);
    else snprintf(buf, sizeof(buf), "Not set up");
    lv_label_set_text(s_wifi_val, buf);
    lv_obj_set_style_text_color(s_wifi_val, st.wifi_connected ? UI_C_OK : UI_C_MUTED, 0);

    const char *ip = st.active == NET_ACTIVE_ETH ? st.eth_ip : st.active == NET_ACTIVE_WIFI ? st.wifi_ip : NULL;
    if (st.portal_active)
        snprintf(buf, sizeof(buf), "Hotspot \"%s\"\nhttp://%s", st.portal_ssid, st.portal_ip);
    else if (ip)
        snprintf(buf, sizeof(buf), "http://%s\nhttp://%s.local", ip, g_settings.hostname);
    else snprintf(buf, sizeof(buf), "Offline");
    lv_label_set_text(s_web_val, buf);
    lv_label_set_text(s_portal_btn_lbl, st.portal_active ? "Stop setup hotspot" : "Start setup hotspot");
}

static void build_network(lv_obj_t *tab)
{
    tab_page(tab);
    ui_section_title(tab, "STATUS");
    lv_obj_t *card = ui_card(tab);
    lv_obj_t *r = ui_row(card, "Ethernet");
    s_eth_val = value_label(r);
    r = ui_row(card, "Wi-Fi");
    s_wifi_val = value_label(r);
    r = ui_row(card, "Web settings");
    s_web_val = value_label(r);

    ui_section_title(tab, "WI-FI");
    lv_obj_t *c2 = ui_card(tab);
    lv_obj_t *br = lv_obj_create(c2);
    lv_obj_remove_style_all(br);
    lv_obj_set_size(br, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(br, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(br, 12, 0);
    lv_obj_t *scan = ui_button(br, LV_SYMBOL_WIFI "  Scan for networks", UI_C_ACCENT);
    lv_obj_set_flex_grow(scan, 1);
    s_scan_btn_lbl = lv_obj_get_child(scan, 0);
    lv_obj_add_event_cb(scan, scan_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *forget = ui_button(br, LV_SYMBOL_TRASH "  Forget", UI_C_SURFACE2);
    lv_obj_add_event_cb(forget, forget_cb, LV_EVENT_CLICKED, NULL);

    s_scan_list = lv_obj_create(c2);
    lv_obj_remove_style_all(s_scan_list);
    lv_obj_set_size(s_scan_list, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(s_scan_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_scan_list, 8, 0);

    ui_section_title(tab, "FIRST-TIME SETUP");
    lv_obj_t *c3 = ui_card(tab);
    lv_obj_t *hint = lv_label_create(c3);
    lv_label_set_text(hint, "The setup hotspot lets a phone or PC join the panel directly and open the settings page, "
                            "even without Ethernet. It starts by itself when no network is available.");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_color(hint, UI_C_MUTED, 0);
    lv_obj_t *pb = ui_button(c3, "Start setup hotspot", UI_C_SURFACE2);
    s_portal_btn_lbl = lv_obj_get_child(pb, 0);
    lv_obj_add_event_cb(pb, portal_cb, LV_EVENT_CLICKED, NULL);

    refresh_network();
}

/* ================================================================== DISPLAY */

static const uint16_t s_saver_vals[] = {0, 30, 60, 120, 300, 600, 1800};
static const char *s_saver_opts = "Off\n30 seconds\n1 minute\n2 minutes\n5 minutes\n10 minutes\n30 minutes";

static void bright_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        ui_backlight_preview(v);
        g_settings.brightness = (uint8_t)v;
    } else {
        ui_settings_changed();
    }
}

static void night_bright_cb(lv_event_t *e)
{
    lv_obj_t *s = lv_event_get_target(e);
    int v = lv_slider_get_value(s);
    if (lv_event_get_code(e) == LV_EVENT_VALUE_CHANGED) {
        ui_backlight_preview(v);
        g_settings.night_brightness = (uint8_t)v;
    } else {
        ui_settings_changed();
    }
}

static void saver_cb(lv_event_t *e)
{
    int i = lv_dropdown_get_selected(lv_event_get_target(e));
    g_settings.screensaver_s = s_saver_vals[i];
    ui_settings_changed();
}

static void saver_mode_cb(lv_event_t *e)
{
    g_settings.saver_mode = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    ui_settings_changed();
}

static void frame_clock_cb(lv_event_t *e)
{
    g_settings.photo_frame_clock = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ui_settings_changed();
}

static void night_sw_cb(lv_event_t *e)
{
    g_settings.night_dim = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ui_settings_changed();
}

static void night_start_cb(lv_event_t *e)
{
    g_settings.night_start_h = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    ui_settings_changed();
}

static void night_end_cb(lv_event_t *e)
{
    g_settings.night_end_h = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    ui_settings_changed();
}

static void build_hour_opts(char *buf, size_t n)
{
    buf[0] = 0;
    for (int h = 0; h < 24; h++) {
        char tmp[16];
        ui_fmt_hour(tmp, sizeof(tmp), h);
        strncat(buf, tmp, n - strlen(buf) - 2);
        if (h < 23) strncat(buf, "\n", n - strlen(buf) - 1);
    }
}

static void build_display(lv_obj_t *tab)
{
    tab_page(tab);
    ui_section_title(tab, "BRIGHTNESS");
    lv_obj_t *c = ui_card(tab);
    mk_slider(c, 5, 100, g_settings.brightness, bright_cb);

    ui_section_title(tab, "SCREENSAVER");
    lv_obj_t *c2 = ui_card(tab);
    lv_obj_t *r = ui_row(c2, "Start after");
    int sel = 0;
    for (int i = 0; i < (int)(sizeof(s_saver_vals) / sizeof(s_saver_vals[0])); i++)
        if (s_saver_vals[i] == g_settings.screensaver_s) sel = i;
    mk_dropdown(r, s_saver_opts, sel, 240, saver_cb);
    r = ui_row(c2, "Screensaver shows");
    mk_dropdown(r, "Clock\nPhoto frame", g_settings.saver_mode, 240, saver_mode_cb);
    r = ui_row(c2, "Time over the photos");
    mk_switch(r, g_settings.photo_frame_clock, frame_clock_cb);
    lv_obj_t *fh = lv_label_create(c2);
    lv_label_set_text_fmt(fh, "Photo frame folder: %s  (change it in Photos > options)",
                          g_settings.photo_dir[0] ? g_settings.photo_dir : "whole card");
    lv_label_set_long_mode(fh, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(fh, lv_pct(100));
    lv_obj_set_style_text_font(fh, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(fh, UI_C_MUTED, 0);

    ui_section_title(tab, "NIGHT DIMMING");
    lv_obj_t *c3 = ui_card(tab);
    r = ui_row(c3, "Dim the screen at night");
    mk_switch(r, g_settings.night_dim, night_sw_cb);
    static char hours[256];
    build_hour_opts(hours, sizeof(hours));
    r = ui_row(c3, "Night starts");
    mk_dropdown(r, hours, g_settings.night_start_h, 200, night_start_cb);
    r = ui_row(c3, "Night ends");
    mk_dropdown(r, hours, g_settings.night_end_h, 200, night_end_cb);
    lv_obj_t *l = lv_label_create(c3);
    lv_label_set_text(l, "Night brightness  (used after 30 s without a touch; tap to brighten)");
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(l, lv_pct(100));
    mk_slider(c3, 1, 100, g_settings.night_brightness, night_bright_cb);
}

/* ================================================================== TIME */

static void fill_cities(int region_idx, const char *select_id)
{
    static char opts[4096];
    opts[0] = 0;
    s_city_count = 0;
    int sel = 0;
    const char *region = g_tz_regions[region_idx];
    for (size_t i = 0; i < g_tz_count && s_city_count < (int)(sizeof(s_city_map) / sizeof(s_city_map[0])); i++) {
        if (strcmp(g_tz_table[i].region, region) != 0) continue;
        if (s_city_count) strncat(opts, "\n", sizeof(opts) - strlen(opts) - 1);
        strncat(opts, g_tz_table[i].city, sizeof(opts) - strlen(opts) - 1);
        if (select_id && strcmp(select_id, g_tz_table[i].id) == 0) sel = s_city_count;
        s_city_map[s_city_count++] = (int)i;
    }
    lv_dropdown_set_options(s_city_dd, opts);
    lv_dropdown_set_selected(s_city_dd, sel);
}

static void refresh_time_info(void)
{
    if (!s_now_lbl) return;
    time_t now = plat_now();
    struct tm t;
    localtime_r(&now, &t);
    char tbuf[32], off[16], abbr[16], line[160];
    ui_fmt_time(tbuf, sizeof(tbuf), &t, true, true);
    ui_fmt_utc_offset(off, sizeof(off), &t);
    strftime(abbr, sizeof(abbr), "%Z", &t);
    if (abbr[0] == '<' || abbr[0] == '+' || abbr[0] == '-') abbr[0] = 0;
    net_status_t st;
    plat_net_get_status(&st);
    char sync[64];
    if (st.time_synced && st.last_sync) {
        long ago = (long)(now - st.last_sync);
        if (ago < 120) snprintf(sync, sizeof(sync), "synced just now");
        else if (ago < 7200) snprintf(sync, sizeof(sync), "synced %ld min ago", ago / 60);
        else snprintf(sync, sizeof(sync), "synced %ld h ago", ago / 3600);
    } else if (st.time_synced) {
        snprintf(sync, sizeof(sync), plat_rtc_restored() ? "from battery clock" : "kept over restart");
    } else {
        snprintf(sync, sizeof(sync), "waiting for network time");
    }
    snprintf(line, sizeof(line), "%s  %s  %s\n%s  -  %s", tbuf, abbr, off, sync, g_settings.ntp_server);
    lv_label_set_text(s_now_lbl, line);
    if (s_rtc_lbl) {
        char rtc[320];
        plat_rtc_status(rtc, sizeof(rtc));
        lv_label_set_text(s_rtc_lbl, rtc);
    }
}

static void update_tz_note(void)
{
    const tz_entry_t *z = tz_find(g_settings.tz_id);
    if (g_settings.tz_custom[0]) {
        lv_label_set_text_fmt(s_tz_note, "Custom rule active from the web page: %s", g_settings.tz_custom);
        lv_obj_remove_flag(s_tz_note, LV_OBJ_FLAG_HIDDEN);
    } else if (z && z->note) {
        lv_label_set_text(s_tz_note, z->note);
        lv_obj_remove_flag(s_tz_note, LV_OBJ_FLAG_HIDDEN);
    } else if (z && !z->dst_abbr) {
        lv_label_set_text(s_tz_note, "This zone does not change clocks seasonally.");
        lv_obj_remove_flag(s_tz_note, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_tz_note, LV_OBJ_FLAG_HIDDEN);
    }
}

static void region_cb(lv_event_t *e)
{
    int r = lv_dropdown_get_selected(lv_event_get_target(e));
    fill_cities(r, NULL);
    int idx = s_city_map[0];
    snprintf(g_settings.tz_id, sizeof(g_settings.tz_id), "%s", g_tz_table[idx].id);
    update_tz_note();
    ui_settings_changed();
    refresh_time_info();
}

static void city_cb(lv_event_t *e)
{
    int c = lv_dropdown_get_selected(lv_event_get_target(e));
    if (c < 0 || c >= s_city_count) return;
    snprintf(g_settings.tz_id, sizeof(g_settings.tz_id), "%s", g_tz_table[s_city_map[c]].id);
    update_tz_note();
    ui_settings_changed();
    refresh_time_info();
}

static void dst_cb(lv_event_t *e)
{
    g_settings.dst_mode = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    ui_settings_changed();
    refresh_time_info();
}

static void h24_cb(lv_event_t *e)
{
    g_settings.clock_24h = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ui_settings_changed();
    refresh_time_info();
}

static void rtc_vbat_cb(lv_event_t *e)
{
    g_settings.rtc_vbat = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ui_settings_changed();
    refresh_time_info();
}

static void secs_cb(lv_event_t *e)
{
    g_settings.clock_seconds = lv_obj_has_state(lv_event_get_target(e), LV_STATE_CHECKED);
    ui_settings_changed();
}

static void face_cb(lv_event_t *e)
{
    g_settings.clock_face = (uint8_t)lv_dropdown_get_selected(lv_event_get_target(e));
    ui_settings_changed();
}

static void build_time(lv_obj_t *tab)
{
    tab_page(tab);
    ui_section_title(tab, "CURRENT TIME");
    lv_obj_t *c0 = ui_card(tab);
    s_now_lbl = lv_label_create(c0);
    lv_obj_set_style_text_font(s_now_lbl, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(s_now_lbl, 6, 0);

    ui_section_title(tab, "TIME ZONE");
    lv_obj_t *c = ui_card(tab);
    static char regions[512];
    regions[0] = 0;
    for (size_t i = 0; i < g_tz_region_count; i++) {
        if (i) strncat(regions, "\n", sizeof(regions) - strlen(regions) - 1);
        strncat(regions, g_tz_regions[i], sizeof(regions) - strlen(regions) - 1);
    }
    const tz_entry_t *z = tz_find(g_settings.tz_id);
    int rsel = 0;
    for (size_t i = 0; z && i < g_tz_region_count; i++)
        if (strcmp(g_tz_regions[i], z->region) == 0) rsel = (int)i;
    lv_obj_t *r = ui_row(c, "Region");
    s_region_dd = mk_dropdown(r, regions, rsel, 400, region_cb);
    r = ui_row(c, "City");
    s_city_dd = mk_dropdown(r, "", 0, 400, city_cb);
    fill_cities(rsel, g_settings.tz_id);
    r = ui_row(c, "Daylight saving");
    s_dst_dd = mk_dropdown(r, "Automatic\nAlways standard time\nAlways daylight time", g_settings.dst_mode, 400, dst_cb);
    s_tz_note = lv_label_create(c);
    lv_label_set_long_mode(s_tz_note, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_tz_note, lv_pct(100));
    lv_obj_set_style_text_color(s_tz_note, UI_C_WARN, 0);
    lv_obj_set_style_text_font(s_tz_note, &lv_font_montserrat_16, 0);
    update_tz_note();

    ui_section_title(tab, "CLOCK");
    lv_obj_t *c2 = ui_card(tab);
    r = ui_row(c2, "24-hour time");
    mk_switch(r, g_settings.clock_24h, h24_cb);
    r = ui_row(c2, "Show seconds");
    mk_switch(r, g_settings.clock_seconds, secs_cb);
    r = ui_row(c2, "Clock style");
    mk_dropdown(r, "Digital\nAnalog", g_settings.clock_face, 240, face_cb);

    ui_section_title(tab, "BATTERY CLOCK");
    lv_obj_t *c3 = ui_card(tab);
    r = ui_row(c3, "Keep time on the RTC battery");
    mk_switch(r, g_settings.rtc_vbat, rtc_vbat_cb);
    s_rtc_lbl = lv_label_create(c3);
    lv_label_set_long_mode(s_rtc_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_rtc_lbl, lv_pct(100));
    lv_obj_set_style_text_font(s_rtc_lbl, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_line_space(s_rtc_lbl, 4, 0);
    lv_obj_set_style_text_color(s_rtc_lbl, UI_C_MUTED, 0);

    refresh_time_info();
}

/* ================================================================== SYSTEM */

static lv_obj_t *s_sys_info;

static void reboot_ok(void *u) { (void)u; plat_reboot(); }
static void reboot_cb(lv_event_t *e)
{
    (void)e;
    ui_confirm("Restart", "Restart the panel now?", "Restart", UI_C_ACCENT, reboot_ok, NULL);
}

static void reset_ok(void *u) { (void)u; plat_factory_reset(); }
static void reset_confirm(void)
{
    ui_confirm("Factory reset",
               "Erase Wi-Fi details, lock codes and all settings, then restart? The relays switch off. "
               "This cannot be undone.",
               "Erase everything", UI_C_ERR, reset_ok, NULL);
}

static void reset_code_ok(const char *code, void *u)
{
    (void)u;
    if (lock_check(code, true) == LOCK_ADMIN) reset_confirm();
    else ui_toast("Wrong admin code");
}

static void reset_cb(lv_event_t *e)
{
    (void)e;
    if (g_lock.admin_set)
        ui_pin_prompt("Admin code", "A factory reset needs the lock admin code", 6, 6, reset_code_ok, NULL);
    else
        reset_confirm();
}

static void refresh_c6(void);
static void c6_update_cb(lv_event_t *e);
static lv_obj_t *s_c6_lbl, *s_c6_btn;

static void build_system(lv_obj_t *tab)
{
    tab_page(tab);
    ui_section_title(tab, "ABOUT");
    lv_obj_t *c = ui_card(tab);
    s_sys_info = lv_label_create(c);
    lv_obj_set_style_text_font(s_sys_info, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_line_space(s_sys_info, 8, 0);
    char buf[512];
    plat_sys_info(buf, sizeof(buf));
    lv_label_set_text(s_sys_info, buf);

    ui_section_title(tab, "WI-FI MODULE (ESP32-C6)");
    lv_obj_t *cw = ui_card(tab);
    s_c6_lbl = lv_label_create(cw);
    lv_label_set_long_mode(s_c6_lbl, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_c6_lbl, lv_pct(100));
    lv_obj_set_style_text_color(s_c6_lbl, UI_C_MUTED, 0);
    s_c6_btn = ui_button(cw, LV_SYMBOL_DOWNLOAD "  Update Wi-Fi module", UI_C_ACCENT);
    lv_obj_add_event_cb(s_c6_btn, c6_update_cb, LV_EVENT_CLICKED, NULL);
    refresh_c6();

    ui_section_title(tab, "MAINTENANCE");
    lv_obj_t *c2 = ui_card(tab);
    lv_obj_t *hint = lv_label_create(c2);
    lv_label_set_text(hint, "Firmware updates can be uploaded from the web settings page (System section).");
    lv_label_set_long_mode(hint, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(hint, lv_pct(100));
    lv_obj_set_style_text_color(hint, UI_C_MUTED, 0);
    lv_obj_t *row = lv_obj_create(c2);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(row, 12, 0);
    lv_obj_t *b1 = ui_button(row, LV_SYMBOL_REFRESH "  Restart", UI_C_SURFACE2);
    lv_obj_set_flex_grow(b1, 1);
    lv_obj_add_event_cb(b1, reboot_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *b2 = ui_button(row, LV_SYMBOL_WARNING "  Factory reset", lv_color_hex(0x5A1E1E));
    lv_obj_set_flex_grow(b2, 1);
    lv_obj_add_event_cb(b2, reset_cb, LV_EVENT_CLICKED, NULL);
}

/* ================================================================== app */

static void refresh_c6(void)
{
    if (!s_c6_lbl) return;
    c6fw_t f;
    plat_c6_fw_status(&f);
    char b[200];
    if (f.running) snprintf(b, sizeof(b), "%s  %d%%\nKeep the panel powered.", f.msg, f.pct);
    else if (!f.link_ok) snprintf(b, sizeof(b), "The Wi-Fi module is not answering.");
    else if (!f.known && f.failed) snprintf(b, sizeof(b), "Installed: too old to report its version. This firmware expects %s.\n%s", f.expected, f.msg);
    else if (!f.known) snprintf(b, sizeof(b), "Installed: too old to report its version. This firmware expects %s.\nUpdate it if Wi-Fi scans or connections fail (needs Ethernet internet).", f.expected);
    else if (f.running) snprintf(b, sizeof(b), "%s  %d%%\nKeep the panel powered.", f.msg, f.pct);
    else if (f.failed) snprintf(b, sizeof(b), "Installed %s, this firmware expects %s.\n%s", f.installed, f.expected, f.msg);
    else if (f.update_available) snprintf(b, sizeof(b), "Installed %s, this firmware expects %s.\nUpdate it if Wi-Fi does not work (needs Ethernet or Wi-Fi internet).", f.installed, f.expected);
    else snprintf(b, sizeof(b), "Installed %s - up to date.", f.installed);
    lv_label_set_text(s_c6_lbl, b);
    if (f.link_ok && (!f.known || f.update_available) && !f.running) lv_obj_remove_flag(s_c6_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_c6_btn, LV_OBJ_FLAG_HIDDEN);
}

static void c6_update_ok(void *u) { (void)u; plat_c6_fw_update(); refresh_c6(); }

static void c6_update_cb(lv_event_t *e)
{
    (void)e;
    ui_confirm("Update Wi-Fi module",
               "Download the matching ESP-Hosted firmware from Espressif and install it on the ESP32-C6, then restart. "
               "Takes about a minute; do not unplug the panel.",
               "Update", UI_C_ACCENT, c6_update_ok, NULL);
}

static void settings_tick(lv_timer_t *t)
{
    (void)t;
    refresh_network();
    refresh_time_info();
    refresh_c6();
}

static void settings_create(lv_obj_t *parent)
{
    s_gen++;
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);
    s_tv = lv_tabview_create(parent);
    lv_obj_set_size(s_tv, lv_pct(100), lv_pct(100));
    lv_tabview_set_tab_bar_size(s_tv, 64);
    lv_obj_set_style_bg_color(s_tv, UI_C_BG, 0);
    lv_obj_t *bar = lv_tabview_get_tab_bar(s_tv);
    lv_obj_set_style_bg_color(bar, UI_C_SURFACE, 0);
    lv_obj_set_style_text_font(bar, &lv_font_montserrat_20, 0);
    lv_obj_t *content = lv_tabview_get_content(s_tv);
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLL_ELASTIC);

    build_network(lv_tabview_add_tab(s_tv, LV_SYMBOL_WIFI "  Network"));
    build_display(lv_tabview_add_tab(s_tv, LV_SYMBOL_IMAGE "  Display"));
    build_time(lv_tabview_add_tab(s_tv, LV_SYMBOL_BELL "  Time"));
    build_system(lv_tabview_add_tab(s_tv, LV_SYMBOL_SETTINGS "  System"));

    /* tabs are switched by tapping only: horizontal swipes would fight the sliders */
    lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);

    s_timer = lv_timer_create(settings_tick, 1000, NULL);
}

static void settings_destroy(void)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    s_tv = NULL;
    s_eth_val = s_wifi_val = s_web_val = s_portal_btn_lbl = s_scan_btn_lbl = s_scan_list = NULL;
    s_region_dd = s_city_dd = s_dst_dd = s_tz_note = s_now_lbl = s_rtc_lbl = NULL;
    s_sys_info = NULL;
    s_c6_lbl = s_c6_btn = NULL;
    s_gen++;
}

static void settings_on_settings(void)
{
    /* Rebuild to reflect values changed from the web page. */
    if (!s_tv) return;
    uint32_t active = lv_tabview_get_tab_active(s_tv);
    lv_obj_t *parent = lv_obj_get_parent(s_tv);
    settings_destroy();
    lv_obj_clean(parent);
    settings_create(parent);
    lv_tabview_set_active(s_tv, active, LV_ANIM_OFF);
}

/* simulator hook: open a given tab */
void app_settings_debug_tab(int idx)
{
    if (s_tv) lv_tabview_set_active(s_tv, idx, LV_ANIM_OFF);
}

lv_obj_t *app_settings_debug_page(int idx)
{
    return s_tv ? lv_obj_get_child(lv_tabview_get_content(s_tv), idx) : NULL;
}

const ui_app_t app_settings = {
    .name = "Settings",
    .icon = &icon_settings,
    .create = settings_create,
    .destroy = settings_destroy,
    .on_settings = settings_on_settings,
};
