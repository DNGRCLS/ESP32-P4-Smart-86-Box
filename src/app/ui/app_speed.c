/* Speed Test app: ping, jitter, download and upload against Cloudflare. */
#include <math.h>
#include <stdio.h>
#include <string.h>
#include "ui_internal.h"

static lv_obj_t *s_arc, *s_big, *s_unit, *s_phase, *s_btn, *s_btn_lbl, *s_info, *s_hist;
static lv_obj_t *s_val[4];
static lv_timer_t *s_timer;
static spd_phase_t s_last_phase = (spd_phase_t)-1;

static const char *const s_tile_names[4] = {"PING", "JITTER", "DOWNLOAD", "UPLOAD"};

/* log-ish gauge: 0..1000 Mbps -> 0..1000 */
static int gauge_val(float mbps)
{
    if (mbps <= 0) return 0;
    float v = log10f(1.0f + mbps) / log10f(1001.0f) * 1000.0f;
    return v > 1000 ? 1000 : (int)v;
}

static void fmt_mbps(char *b, size_t n, float v)
{
    if (v <= 0) snprintf(b, n, "-");
    else if (v < 10) snprintf(b, n, "%.2f", v);
    else if (v < 100) snprintf(b, n, "%.1f", v);
    else snprintf(b, n, "%.0f", v);
}

static void fill_history(void)
{
    lv_obj_clean(s_hist);
    speed_hist_t h[6];
    int n = plat_speed_history(h, 6);
    if (!n) {
        lv_obj_t *l = lv_label_create(s_hist);
        lv_label_set_text(l, "No tests yet");
        lv_obj_set_style_text_color(l, UI_C_MUTED, 0);
        return;
    }
    for (int i = 0; i < n; i++) {
        struct tm tm;
        localtime_r(&h[i].t, &tm);
        char d[16], t[16], dn[16], up[16], line[128];
        strftime(d, sizeof(d), "%b %d", &tm);
        ui_fmt_time(t, sizeof(t), &tm, false, true);
        fmt_mbps(dn, sizeof(dn), h[i].down_mbps);
        fmt_mbps(up, sizeof(up), h[i].up_mbps);
        snprintf(line, sizeof(line), "%s %s   %s   " LV_SYMBOL_DOWN " %s   " LV_SYMBOL_UP " %s Mbps   %.0f ms", d, t,
                 h[i].iface, dn, up, h[i].ping_ms);
        lv_obj_t *l = lv_label_create(s_hist);
        lv_label_set_text(l, line);
        lv_obj_set_style_text_font(l, &lv_font_montserrat_16, 0);
        lv_obj_set_style_text_color(l, UI_C_MUTED, 0);
    }
}

static void refresh(void)
{
    if (!s_arc) return;
    speedtest_t st;
    plat_speedtest_get(&st);
    char b[32];
    bool running = st.phase >= SPD_PREPARE && st.phase <= SPD_UPLOAD;

    const char *ph = "";
    lv_color_t arc_c = UI_C_ACCENT;
    switch (st.phase) {
    case SPD_IDLE: ph = "Ready"; break;
    case SPD_PREPARE: ph = "Finding the nearest server..."; break;
    case SPD_PING: ph = "Measuring ping"; break;
    case SPD_DOWNLOAD: ph = LV_SYMBOL_DOWN "  Download"; break;
    case SPD_UPLOAD: ph = LV_SYMBOL_UP "  Upload"; arc_c = lv_color_hex(0xA371F7); break;
    case SPD_DONE: ph = "Complete"; break;
    case SPD_ERROR: ph = st.error[0] ? st.error : "Test failed"; break;
    }
    lv_label_set_text(s_phase, ph);
    lv_obj_set_style_text_color(s_phase, st.phase == SPD_ERROR ? UI_C_ERR : UI_C_MUTED, 0);
    lv_obj_set_style_arc_color(s_arc, arc_c, LV_PART_INDICATOR);

    float big = 0;
    if (st.phase == SPD_DOWNLOAD || st.phase == SPD_UPLOAD) big = st.live_mbps;
    else if (st.phase == SPD_DONE) big = st.down_mbps;
    if (st.phase == SPD_PING) {
        lv_label_set_text(s_big, st.ping_ms > 0 ? (snprintf(b, sizeof(b), "%.0f", st.ping_ms), b) : "...");
        lv_label_set_text(s_unit, "ms");
        lv_arc_set_value(s_arc, 0);
    } else {
        fmt_mbps(b, sizeof(b), big);
        lv_label_set_text(s_big, big > 0 ? b : "0");
        lv_label_set_text(s_unit, "Mbps");
        lv_arc_set_value(s_arc, gauge_val(big));
    }

    if (st.ping_ms > 0) { snprintf(b, sizeof(b), "%.0f ms", st.ping_ms); lv_label_set_text(s_val[0], b); }
    else lv_label_set_text(s_val[0], "-");
    if (st.jitter_ms > 0 || st.phase > SPD_PING) { snprintf(b, sizeof(b), "%.1f ms", st.jitter_ms); lv_label_set_text(s_val[1], st.ping_ms > 0 ? b : "-"); }
    else lv_label_set_text(s_val[1], "-");
    fmt_mbps(b, sizeof(b), st.down_mbps);
    lv_label_set_text(s_val[2], b);
    fmt_mbps(b, sizeof(b), st.up_mbps);
    lv_label_set_text(s_val[3], b);

    if (st.iface[0] || st.server[0]) {
        char info[160];
        snprintf(info, sizeof(info), "via %s   Cloudflare %s   %s", st.iface[0] ? st.iface : "-", st.server, st.ip);
        lv_label_set_text(s_info, info);
    } else {
        net_status_t ns;
        plat_net_get_status(&ns);
        lv_label_set_text(s_info, ns.active == NET_ACTIVE_ETH ? "Tests will run over Ethernet"
                                  : ns.active == NET_ACTIVE_WIFI ? "Tests will run over Wi-Fi"
                                                                 : LV_SYMBOL_WARNING "  No network connection");
    }
    lv_label_set_text(s_btn_lbl, running ? LV_SYMBOL_STOP "  Stop" : st.phase == SPD_DONE ? LV_SYMBOL_REFRESH "  Test again" : LV_SYMBOL_PLAY "  Start test");
    lv_obj_set_style_bg_color(s_btn, running ? UI_C_SURFACE2 : UI_C_ACCENT, 0);

    if (st.phase != s_last_phase && (st.phase == SPD_DONE || s_last_phase == (spd_phase_t)-1)) fill_history();
    s_last_phase = st.phase;
}

static void tick_cb(lv_timer_t *t) { (void)t; refresh(); }

static void btn_cb(lv_event_t *e)
{
    (void)e;
    speedtest_t st;
    plat_speedtest_get(&st);
    if (st.phase >= SPD_PREPARE && st.phase <= SPD_UPLOAD) plat_speedtest_cancel();
    else plat_speedtest_start();
    refresh();
}

static void speed_create(lv_obj_t *parent)
{
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 16, 0);
    lv_obj_set_style_pad_row(parent, 12, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *top = lv_obj_create(parent);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), 340);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    s_arc = lv_arc_create(top);
    lv_obj_set_size(s_arc, 330, 330);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 1000);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 22, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 22, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, UI_C_SURFACE2, LV_PART_MAIN);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);

    s_big = lv_label_create(s_arc);
    lv_obj_set_style_text_font(s_big, ui_font(UI_FW_LIGHT, 84), 0);
    lv_obj_align(s_big, LV_ALIGN_CENTER, 0, -10);
    s_unit = lv_label_create(s_arc);
    lv_obj_set_style_text_font(s_unit, ui_font(UI_FW_REGULAR, 24), 0);
    lv_obj_set_style_text_color(s_unit, UI_C_MUTED, 0);
    lv_obj_align(s_unit, LV_ALIGN_CENTER, 0, 52);
    s_phase = lv_label_create(s_arc);
    lv_obj_set_width(s_phase, 240);
    lv_label_set_long_mode(s_phase, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_align(s_phase, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(s_phase, &lv_font_montserrat_16, 0);
    lv_obj_align(s_phase, LV_ALIGN_BOTTOM_MID, 0, -6);

    lv_obj_t *tiles = lv_obj_create(top);
    lv_obj_remove_style_all(tiles);
    lv_obj_set_size(tiles, 340, 330);
    lv_obj_set_flex_flow(tiles, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(tiles, 12, 0);
    lv_obj_set_style_pad_row(tiles, 12, 0);
    for (int i = 0; i < 4; i++) {
        lv_obj_t *t = lv_obj_create(tiles);
        lv_obj_set_size(t, 164, 159);
        lv_obj_set_style_bg_color(t, UI_C_SURFACE, 0);
        lv_obj_set_style_border_color(t, UI_C_BORDER, 0);
        lv_obj_set_style_radius(t, 18, 0);
        lv_obj_remove_flag(t, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_t *n = lv_label_create(t);
        lv_label_set_text(n, s_tile_names[i]);
        lv_obj_set_style_text_font(n, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(n, UI_C_MUTED, 0);
        lv_obj_align(n, LV_ALIGN_TOP_LEFT, 0, 0);
        s_val[i] = lv_label_create(t);
        lv_label_set_text(s_val[i], "-");
        lv_obj_set_style_text_font(s_val[i], ui_font(UI_FW_MEDIUM, i < 2 ? 30 : 40), 0);
        lv_obj_align(s_val[i], LV_ALIGN_LEFT_MID, 0, 6);
        if (i >= 2) {
            lv_obj_t *u = lv_label_create(t);
            lv_label_set_text(u, "Mbps");
            lv_obj_set_style_text_color(u, UI_C_MUTED, 0);
            lv_obj_align(u, LV_ALIGN_BOTTOM_LEFT, 0, 0);
        }
    }

    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);
    s_btn = ui_button(row, LV_SYMBOL_PLAY "  Start test", UI_C_ACCENT);
    lv_obj_set_size(s_btn, 220, 64);
    s_btn_lbl = lv_obj_get_child(s_btn, 0);
    lv_obj_add_event_cb(s_btn, btn_cb, LV_EVENT_CLICKED, NULL);
    s_info = lv_label_create(row);
    lv_label_set_long_mode(s_info, LV_LABEL_LONG_WRAP);
    lv_obj_set_flex_grow(s_info, 1);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_info, UI_C_MUTED, 0);

    ui_section_title(parent, "RECENT RESULTS");
    s_hist = lv_obj_create(parent);
    lv_obj_remove_style_all(s_hist);
    lv_obj_set_width(s_hist, lv_pct(100));
    lv_obj_set_flex_grow(s_hist, 1);
    lv_obj_set_flex_flow(s_hist, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_hist, 6, 0);
    lv_obj_set_scroll_dir(s_hist, LV_DIR_VER);

    s_last_phase = (spd_phase_t)-1;
    refresh();
    s_timer = lv_timer_create(tick_cb, 200, NULL);
}

static void speed_destroy(void)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    s_arc = NULL;
}

const ui_app_t app_speedtest = {
    .name = "Speed Test",
    .icon = &icon_speed,
    .create = speed_create,
    .destroy = speed_destroy,
};
