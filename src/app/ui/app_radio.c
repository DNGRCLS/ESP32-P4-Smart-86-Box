/* Web Radio app: now-playing card, station list, search, station editing. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_internal.h"
#include "../core/stations.h"

static lv_obj_t *s_name, *s_title, *s_info, *s_chip, *s_play_lbl, *s_vol, *s_save_btn, *s_out_btn;
static lv_obj_t *s_list, *s_count_lbl;
static lv_obj_t *s_rows[STATIONS_MAX];
static lv_timer_t *s_timer;
static int s_hl = -2;                 /* highlighted row */
static radio_state_t s_last_state = (radio_state_t)-1;
static char s_last_title[160];
static char s_pending_url[256];
static uint32_t s_gen;

/* search results */
static radio_search_rec_t *s_results;
static int s_result_count;
static lv_obj_t *s_results_bd;

static void rebuild_rows(void);

/* ------------------------------------------------------------------ helpers */

static void host_of(const char *url, char *out, size_t n)
{
    const char *p = strstr(url, "://");
    p = p ? p + 3 : url;
    size_t i = 0;
    while (p[i] && p[i] != '/' && p[i] != ':' && i < n - 1) { out[i] = p[i]; i++; }
    out[i] = 0;
}

static int current_index(const radio_status_t *st)
{
    if (st->state != RADIO_IDLE && st->url[0]) return stations_find_url(&g_stations, st->url);
    return -1;
}

static void play_index(int i)
{
    if (g_stations.count == 0) { ui_toast("No stations - add one or search first"); return; }
    if (i < 0) i = 0;
    i %= g_stations.count;
    plat_radio_play(g_stations.items[i].name, g_stations.items[i].url);
    g_settings.radio_last = (int16_t)i;
}

static void stations_changed(void)
{
    plat_stations_save();
    if (g_settings.radio_last >= g_stations.count) g_settings.radio_last = g_stations.count ? 0 : -1;
    ui_settings_changed();
    rebuild_rows();
}

/* ------------------------------------------------------------------ now playing */

static void refresh(void)
{
    if (!s_name) return;
    radio_status_t st;
    plat_radio_get_status(&st);

    const char *chip = "";
    lv_color_t cc = UI_C_MUTED;
    switch (st.state) {
    case RADIO_IDLE: chip = "STOPPED"; break;
    case RADIO_CONNECTING: chip = "CONNECTING"; cc = UI_C_WARN; break;
    case RADIO_BUFFERING: chip = "BUFFERING"; cc = UI_C_WARN; break;
    case RADIO_PLAYING: chip = "LIVE"; cc = UI_C_OK; break;
    case RADIO_RECONNECTING: chip = "RECONNECTING"; cc = UI_C_WARN; break;
    case RADIO_ERROR: chip = "ERROR"; cc = UI_C_ERR; break;
    }
    lv_label_set_text(s_chip, chip);
    lv_obj_set_style_text_color(s_chip, cc, 0);
    lv_obj_set_style_border_color(s_chip, cc, 0);

    if (st.state == RADIO_IDLE) {
        int i = g_settings.radio_last;
        if (i >= 0 && i < g_stations.count) lv_label_set_text(s_name, g_stations.items[i].name);
        else lv_label_set_text(s_name, "Web Radio");
        lv_label_set_text(s_title, "Tap play to start");
        lv_label_set_text(s_info, "");
    } else {
        lv_label_set_text(s_name, st.name[0] ? st.name : st.url);
        const char *t = st.title[0] ? st.title : (st.state == RADIO_ERROR ? st.error : "");
        if (strcmp(t, s_last_title) != 0 || st.state != s_last_state) {
            snprintf(s_last_title, sizeof(s_last_title), "%s", t);
            lv_label_set_text(s_title, t[0] ? t : " ");
        }
        char info[160] = "";
        int o = 0;
        if (st.codec[0]) o += snprintf(info + o, sizeof(info) - o, "%s", st.codec);
        if (st.bitrate_kbps) o += snprintf(info + o, sizeof(info) - o, "   %d kbps", st.bitrate_kbps);
        if (st.sample_rate) o += snprintf(info + o, sizeof(info) - o, "   %.1f kHz", st.sample_rate / 1000.0);
        if (st.state == RADIO_PLAYING || st.state == RADIO_BUFFERING)
            o += snprintf(info + o, sizeof(info) - o, "   buffer %d%%", st.buffer_pct);
        if (st.state == RADIO_RECONNECTING && st.error[0]) o += snprintf(info + o, sizeof(info) - o, "   %s", st.error);
        audio_out_t ao;
        plat_audio_out_get(&ao);
        if (ao.active) snprintf(info + o, sizeof(info) - o, "   " LV_SYMBOL_BLUETOOTH " %s", ao.name);
        lv_label_set_text(s_info, info);
    }
    {
        audio_out_t ao;
        plat_audio_out_get(&ao);
        lv_obj_t *l = lv_obj_get_child(s_out_btn, 0);
        lv_obj_set_style_bg_color(s_out_btn, ao.active ? UI_C_ACCENT : UI_C_SURFACE2, 0);
        lv_obj_set_style_text_color(l, ao.active ? lv_color_white() : ao.connected && ao.prefer_usb ? UI_C_ACCENT : UI_C_MUTED, 0);
        lv_label_set_text(l, ao.connected && ao.prefer_usb ? LV_SYMBOL_BLUETOOTH : LV_SYMBOL_VOLUME_MAX);
    }
    s_last_state = st.state;
    lv_label_set_text(s_play_lbl, st.state == RADIO_IDLE || st.state == RADIO_ERROR ? LV_SYMBOL_PLAY : LV_SYMBOL_STOP);

    bool unsaved = st.state != RADIO_IDLE && st.url[0] && stations_find_url(&g_stations, st.url) < 0;
    if (unsaved) lv_obj_remove_flag(s_save_btn, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_save_btn, LV_OBJ_FLAG_HIDDEN);

    int hl = current_index(&st);
    if (hl != s_hl) {
        for (int i = 0; i < g_stations.count && s_rows[i]; i++) {
            bool on = i == hl;
            lv_obj_set_style_bg_color(s_rows[i], on ? lv_color_hex(0x173055) : UI_C_SURFACE, 0);
            lv_obj_set_style_border_color(s_rows[i], on ? UI_C_ACCENT : UI_C_BORDER, 0);
            lv_obj_t *ic = lv_obj_get_child(s_rows[i], 0);
            lv_label_set_text(ic, on ? LV_SYMBOL_VOLUME_MAX : LV_SYMBOL_AUDIO);
            lv_obj_set_style_text_color(ic, on ? UI_C_ACCENT : UI_C_MUTED, 0);
        }
        s_hl = hl;
    }
}

static void tick_cb(lv_timer_t *t)
{
    (void)t;
    refresh();
}

static void play_cb(lv_event_t *e)
{
    (void)e;
    radio_status_t st;
    plat_radio_get_status(&st);
    if (st.state == RADIO_IDLE || st.state == RADIO_ERROR) play_index(g_settings.radio_last);
    else plat_radio_stop();
    refresh();
}

static void step_cb(lv_event_t *e)
{
    int dir = (int)(intptr_t)lv_event_get_user_data(e);
    if (g_stations.count == 0) return;
    radio_status_t st;
    plat_radio_get_status(&st);
    int i = current_index(&st);
    if (i < 0) i = g_settings.radio_last;
    if (i < 0) i = 0;
    i = (i + dir + g_stations.count) % g_stations.count;
    play_index(i);
    refresh();
}

static void out_pick(int pick, void *u)
{
    (void)u;
    if (pick < 0) return;
    plat_audio_out_prefer_usb(pick == 1);
    audio_out_t ao;
    plat_audio_out_get(&ao);
    if (pick == 1 && !ao.connected) ui_toast("Plug the USB / Bluetooth transmitter into the USB-C OTG port");
    else ui_toast(pick == 1 ? "Playing on the USB / Bluetooth device" : "Playing on the built-in speaker");
}

static void out_cb(lv_event_t *e)
{
    (void)e;
    audio_out_t ao;
    plat_audio_out_get(&ao);
    static char i0[64], i1[96];
    static const char *items[2];
    snprintf(i0, sizeof(i0), "%s" LV_SYMBOL_VOLUME_MAX "  Built-in speaker", ao.prefer_usb ? "" : LV_SYMBOL_OK " ");
    if (ao.connected) snprintf(i1, sizeof(i1), "%s" LV_SYMBOL_BLUETOOTH "  %s", ao.prefer_usb ? LV_SYMBOL_OK " " : "", ao.name);
    else snprintf(i1, sizeof(i1), "%s" LV_SYMBOL_BLUETOOTH "  USB / Bluetooth (not plugged in)", ao.prefer_usb ? LV_SYMBOL_OK " " : "");
    items[0] = i0;
    items[1] = i1;
    ui_menu("Play the radio on", items, 2, out_pick, NULL);
}

static void vol_cb(lv_event_t *e)
{
    int v = lv_slider_get_value(lv_event_get_target(e));
    plat_radio_set_volume(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) ui_settings_changed();
}

static void save_name_ok(const char *name, void *u)
{
    (void)u;
    radio_status_t st;
    plat_radio_get_status(&st);
    if (!stations_add(&g_stations, name[0] ? name : st.name, st.url)) { ui_toast("Station list is full"); return; }
    stations_changed();
    ui_toast("Saved to your stations");
}

static void save_cb(lv_event_t *e)
{
    (void)e;
    radio_status_t st;
    plat_radio_get_status(&st);
    ui_text_prompt("Name this station", st.name, false, 63, save_name_ok, NULL);
}

static lv_obj_t *round_btn(lv_obj_t *parent, const char *sym, int size, lv_color_t bg)
{
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_size(b, size, size);
    lv_obj_set_style_radius(b, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(b, bg, 0);
    lv_obj_set_style_shadow_width(b, 0, 0);
    lv_obj_t *l = lv_label_create(b);
    lv_label_set_text(l, sym);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_center(l);
    return b;
}

static void build_now_playing(lv_obj_t *parent)
{
    lv_obj_t *card = ui_card(parent);
    lv_obj_set_style_pad_row(card, 6, 0);

    lv_obj_t *top = lv_obj_create(card);
    lv_obj_remove_style_all(top);
    lv_obj_set_size(top, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(top, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(top, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    s_name = lv_label_create(top);
    lv_obj_set_size(s_name, 470, 46);
    lv_label_set_long_mode(s_name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_name, ui_font(UI_FW_MEDIUM, 30), 0);
    s_chip = lv_label_create(top);
    lv_obj_set_style_text_font(s_chip, &lv_font_montserrat_14, 0);
    lv_obj_set_style_border_width(s_chip, 1, 0);
    lv_obj_set_style_radius(s_chip, 12, 0);
    lv_obj_set_style_pad_hor(s_chip, 10, 0);
    lv_obj_set_style_pad_ver(s_chip, 4, 0);

    s_title = lv_label_create(card);
    lv_obj_set_width(s_title, lv_pct(100));
    lv_label_set_long_mode(s_title, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_set_style_text_font(s_title, ui_font(UI_FW_REGULAR, 24), 0);
    lv_obj_set_style_text_color(s_title, lv_color_hex(0xC9D3DE), 0);

    s_info = lv_label_create(card);
    lv_obj_set_style_text_font(s_info, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_color(s_info, UI_C_MUTED, 0);

    lv_obj_t *ctl = lv_obj_create(card);
    lv_obj_remove_style_all(ctl);
    lv_obj_set_size(ctl, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(ctl, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(ctl, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(ctl, 12, 0);
    lv_obj_set_style_pad_top(ctl, 8, 0);

    lv_obj_t *prev = round_btn(ctl, LV_SYMBOL_PREV, 64, UI_C_SURFACE2);
    lv_obj_add_event_cb(prev, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)-1);
    lv_obj_t *play = round_btn(ctl, LV_SYMBOL_PLAY, 84, UI_C_ACCENT);
    s_play_lbl = lv_obj_get_child(play, 0);
    lv_obj_set_style_text_font(s_play_lbl, &lv_font_montserrat_32, 0);
    lv_obj_add_event_cb(play, play_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *next = round_btn(ctl, LV_SYMBOL_NEXT, 64, UI_C_SURFACE2);
    lv_obj_add_event_cb(next, step_cb, LV_EVENT_CLICKED, (void *)(intptr_t)1);
    s_out_btn = round_btn(ctl, LV_SYMBOL_VOLUME_MAX, 64, UI_C_SURFACE2);
    lv_obj_add_event_cb(s_out_btn, out_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *vl = lv_label_create(ctl);
    lv_label_set_text(vl, LV_SYMBOL_VOLUME_MID);
    lv_obj_set_style_text_color(vl, UI_C_MUTED, 0);
    lv_obj_set_style_pad_left(vl, 10, 0);
    s_vol = lv_slider_create(ctl);
    lv_obj_set_flex_grow(s_vol, 1);
    lv_obj_set_height(s_vol, 14);
    lv_slider_set_range(s_vol, 0, 100);
    lv_slider_set_value(s_vol, g_settings.radio_volume, LV_ANIM_OFF);
    lv_obj_set_ext_click_area(s_vol, 22);
    lv_obj_add_event_cb(s_vol, vol_cb, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_add_event_cb(s_vol, vol_cb, LV_EVENT_RELEASED, NULL);

    s_save_btn = ui_button(ctl, LV_SYMBOL_PLUS " Save", UI_C_SURFACE2);
    lv_obj_set_height(s_save_btn, 52);
    lv_obj_set_style_pad_hor(s_save_btn, 14, 0);
    lv_obj_add_event_cb(s_save_btn, save_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_save_btn, LV_OBJ_FLAG_HIDDEN);
}

/* ------------------------------------------------------------------ station list */

static int s_menu_idx;

static void rename_ok(const char *text, void *u)
{
    int i = (int)(intptr_t)u;
    if (i < 0 || i >= g_stations.count || !text[0]) return;
    snprintf(g_stations.items[i].name, sizeof(g_stations.items[i].name), "%s", text);
    stations_changed();
}

static void url_ok(const char *text, void *u)
{
    int i = (int)(intptr_t)u;
    if (i < 0 || i >= g_stations.count) return;
    if (!stations_url_valid(text)) { ui_toast("Address must start with http:// or https://"); return; }
    snprintf(g_stations.items[i].url, sizeof(g_stations.items[i].url), "%s", text);
    stations_changed();
}

static void delete_ok(void *u)
{
    stations_delete(&g_stations, (int)(intptr_t)u);
    stations_changed();
}

static void row_menu_pick(int pick, void *u)
{
    (void)u;
    int i = s_menu_idx;
    if (i < 0 || i >= g_stations.count) return;
    switch (pick) {
    case 0: play_index(i); break;
    case 1: ui_text_prompt("Station name", g_stations.items[i].name, false, 63, rename_ok, (void *)(intptr_t)i); break;
    case 2: ui_text_prompt("Stream address", g_stations.items[i].url, false, 255, url_ok, (void *)(intptr_t)i); break;
    case 3: stations_move(&g_stations, i, -1); stations_changed(); break;
    case 4: stations_move(&g_stations, i, +1); stations_changed(); break;
    case 5: {
        char msg[128];
        snprintf(msg, sizeof(msg), "Remove \"%s\" from your stations?", g_stations.items[i].name);
        ui_confirm("Delete station", msg, "Delete", UI_C_ERR, delete_ok, (void *)(intptr_t)i);
        break;
    }
    }
}

static void row_long_cb(lv_event_t *e)
{
    s_menu_idx = (int)(intptr_t)lv_event_get_user_data(e);
    static const char *const items[] = {LV_SYMBOL_PLAY "  Play", LV_SYMBOL_EDIT "  Rename",
                                        LV_SYMBOL_SHUFFLE "  Change address", LV_SYMBOL_UP "  Move up",
                                        LV_SYMBOL_DOWN "  Move down", LV_SYMBOL_TRASH "  Delete"};
    ui_menu(g_stations.items[s_menu_idx].name, items, 6, row_menu_pick, NULL);
}

static void row_click_cb(lv_event_t *e)
{
    lv_indev_t *in = lv_indev_active();
    if (in && lv_indev_get_scroll_obj(in)) return;   /* was a scroll, not a tap */
    play_index((int)(intptr_t)lv_event_get_user_data(e));
    refresh();
}

static void rebuild_rows(void)
{
    if (!s_list) return;
    lv_obj_clean(s_list);
    memset(s_rows, 0, sizeof(s_rows));
    s_hl = -2;
    lv_label_set_text_fmt(s_count_lbl, "STATIONS  %d", g_stations.count);
    for (int i = 0; i < g_stations.count; i++) {
        lv_obj_t *r = lv_button_create(s_list);
        s_rows[i] = r;
        lv_obj_set_size(r, lv_pct(100), 66);
        lv_obj_set_style_bg_color(r, UI_C_SURFACE, 0);
        lv_obj_set_style_bg_color(r, UI_C_SURFACE2, LV_STATE_PRESSED);
        lv_obj_set_style_border_width(r, 1, 0);
        lv_obj_set_style_border_color(r, UI_C_BORDER, 0);
        lv_obj_set_style_radius(r, 14, 0);
        lv_obj_set_style_shadow_width(r, 0, 0);
        lv_obj_set_style_pad_hor(r, 16, 0);
        lv_obj_add_event_cb(r, row_click_cb, LV_EVENT_SHORT_CLICKED, (void *)(intptr_t)i);
        lv_obj_add_event_cb(r, row_long_cb, LV_EVENT_LONG_PRESSED, (void *)(intptr_t)i);

        lv_obj_t *ic = lv_label_create(r);
        lv_label_set_text(ic, LV_SYMBOL_AUDIO);
        lv_obj_set_style_text_color(ic, UI_C_MUTED, 0);
        lv_obj_align(ic, LV_ALIGN_LEFT_MID, 0, 0);

        lv_obj_t *n = lv_label_create(r);
        lv_label_set_text(n, g_stations.items[i].name);
        lv_label_set_long_mode(n, LV_LABEL_LONG_DOT);
        lv_obj_set_size(n, 400, 34);
        lv_obj_set_style_text_font(n, ui_font(UI_FW_REGULAR, 22), 0);
        lv_obj_set_style_text_color(n, UI_C_TEXT, 0);
        lv_obj_align(n, LV_ALIGN_LEFT_MID, 38, 0);

        char host[64];
        host_of(g_stations.items[i].url, host, sizeof(host));
        lv_obj_t *h = lv_label_create(r);
        lv_label_set_text(h, host);
        lv_label_set_long_mode(h, LV_LABEL_LONG_DOT);
        lv_obj_set_size(h, 180, 18);
        lv_obj_set_style_text_align(h, LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_set_style_text_font(h, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(h, UI_C_MUTED, 0);
        lv_obj_align(h, LV_ALIGN_RIGHT_MID, 0, 0);
    }
    if (g_stations.count == 0) {
        lv_obj_t *l = lv_label_create(s_list);
        lv_label_set_text(l, "No stations yet. Use Search or Add.");
        lv_obj_set_style_text_color(l, UI_C_MUTED, 0);
    }
    refresh();
}

/* ------------------------------------------------------------------ add / search */

static void add_name_ok(const char *name, void *u)
{
    (void)u;
    if (!stations_add(&g_stations, name, s_pending_url)) { ui_toast("Station list is full"); return; }
    stations_changed();
    play_index(g_stations.count - 1);
}

static void add_url_ok(const char *url, void *u)
{
    (void)u;
    if (!stations_url_valid(url)) { ui_toast("Address must start with http:// or https://"); return; }
    snprintf(s_pending_url, sizeof(s_pending_url), "%s", url);
    ui_text_prompt("Name for this station", "", false, 63, add_name_ok, NULL);
}

static void add_cb(lv_event_t *e)
{
    (void)e;
    ui_text_prompt("Stream address (http://...)", "http://", false, 255, add_url_ok, NULL);
}

static void results_close(void)
{
    if (s_results_bd) lv_obj_delete_async(s_results_bd);
    s_results_bd = NULL;
}

static void results_deleted_cb(lv_event_t *e)
{
    if (s_results_bd == lv_event_get_current_target(e)) s_results_bd = NULL;
}

static void results_close_cb(lv_event_t *e) { (void)e; results_close(); }

static void result_play_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_result_count) return;
    plat_radio_play(s_results[i].name, s_results[i].url);
    ui_toast("Playing - tap Save on the radio screen to keep it");
    results_close();
}

static void result_add_cb(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    if (i < 0 || i >= s_result_count) return;
    if (stations_find_url(&g_stations, s_results[i].url) >= 0) { ui_toast("Already in your stations"); return; }
    if (!stations_add(&g_stations, s_results[i].name, s_results[i].url)) { ui_toast("Station list is full"); return; }
    stations_changed();
    lv_obj_t *b = lv_event_get_current_target(e);
    lv_label_set_text(lv_obj_get_child(b, 0), LV_SYMBOL_OK);
    lv_obj_set_style_bg_color(b, UI_C_OK, 0);
    ui_toast("Added to your stations");
}

static void search_done(const radio_search_rec_t *recs, int n, const char *err, void *user)
{
    if ((uint32_t)(uintptr_t)user != s_gen || !s_list) return;
    if (err) { ui_toast(err); return; }
    free(s_results);
    s_results = malloc(sizeof(radio_search_rec_t) * n);
    memcpy(s_results, recs, sizeof(radio_search_rec_t) * n);
    s_result_count = n;

    results_close();
    lv_obj_t *bd = lv_obj_create(lv_layer_top());
    s_results_bd = bd;
    lv_obj_add_event_cb(bd, results_deleted_cb, LV_EVENT_DELETE, NULL);
    lv_obj_remove_style_all(bd);
    lv_obj_set_size(bd, UI_W, UI_H);
    lv_obj_set_style_bg_color(bd, UI_C_BG, 0);
    lv_obj_set_style_bg_opa(bd, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(bd, UI_C_TEXT, 0);
    lv_obj_add_flag(bd, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(bd, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(bd, 16, 0);
    lv_obj_set_style_pad_row(bd, 10, 0);
    lv_obj_remove_flag(bd, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *hdr = lv_obj_create(bd);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), 56);
    lv_obj_t *t = lv_label_create(hdr);
    lv_label_set_text_fmt(t, "%d stations found", n);
    lv_obj_set_style_text_font(t, ui_font(UI_FW_MEDIUM, 28), 0);
    lv_obj_align(t, LV_ALIGN_LEFT_MID, 4, 0);
    lv_obj_t *close = ui_button(hdr, LV_SYMBOL_CLOSE "  Close", UI_C_SURFACE2);
    lv_obj_align(close, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_add_event_cb(close, results_close_cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *list = lv_obj_create(bd);
    lv_obj_remove_style_all(list);
    lv_obj_set_width(list, lv_pct(100));
    lv_obj_set_flex_grow(list, 1);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(list, 8, 0);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);

    for (int i = 0; i < n; i++) {
        lv_obj_t *r = lv_obj_create(list);
        lv_obj_set_size(r, lv_pct(100), 78);
        lv_obj_set_style_bg_color(r, UI_C_SURFACE, 0);
        lv_obj_set_style_border_color(r, UI_C_BORDER, 0);
        lv_obj_set_style_radius(r, 14, 0);
        lv_obj_set_style_pad_hor(r, 14, 0);
        lv_obj_set_style_pad_ver(r, 6, 0);
        lv_obj_remove_flag(r, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *nm = lv_label_create(r);
        lv_label_set_text(nm, recs[i].name);
        lv_label_set_long_mode(nm, LV_LABEL_LONG_DOT);
        lv_obj_set_size(nm, 480, 34);
        lv_obj_set_style_text_font(nm, ui_font(UI_FW_REGULAR, 22), 0);
        lv_obj_align(nm, LV_ALIGN_TOP_LEFT, 0, 2);

        char sub[128];
        snprintf(sub, sizeof(sub), "%s%s%s %d kbps   %s", recs[i].country, recs[i].country[0] ? "   " : "",
                 recs[i].codec, recs[i].bitrate, recs[i].tags);
        lv_obj_t *sb = lv_label_create(r);
        lv_label_set_text(sb, sub);
        lv_label_set_long_mode(sb, LV_LABEL_LONG_DOT);
        lv_obj_set_size(sb, 480, 18);
        lv_obj_set_style_text_font(sb, &lv_font_montserrat_14, 0);
        lv_obj_set_style_text_color(sb, UI_C_MUTED, 0);
        lv_obj_align(sb, LV_ALIGN_BOTTOM_LEFT, 0, -2);

        lv_obj_t *add = round_btn(r, LV_SYMBOL_PLUS, 54, UI_C_SURFACE2);
        lv_obj_align(add, LV_ALIGN_RIGHT_MID, 0, 0);
        lv_obj_add_event_cb(add, result_add_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        lv_obj_t *pl = round_btn(r, LV_SYMBOL_PLAY, 54, UI_C_ACCENT);
        lv_obj_align(pl, LV_ALIGN_RIGHT_MID, -66, 0);
        lv_obj_add_event_cb(pl, result_play_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
    }
}

static void search_ok(const char *q, void *u)
{
    (void)u;
    if (!q[0]) return;
    ui_toast("Searching Radio Browser...");
    plat_radio_search(q, search_done, (void *)(uintptr_t)s_gen);
}

static void search_cb(lv_event_t *e)
{
    (void)e;
    ui_text_prompt("Search stations (name, city or genre)", "", false, 60, search_ok, NULL);
}

static void reset_ok(void *u)
{
    (void)u;
    stations_set_defaults(&g_stations);
    stations_changed();
}

static void more_pick(int pick, void *u)
{
    (void)u;
    if (pick == 0) {
        g_settings.radio_resume = !g_settings.radio_resume;
        ui_settings_changed();
        ui_toast(g_settings.radio_resume ? "The radio will resume after a restart"
                                         : "The radio stays off after a restart");
    } else if (pick == 1) {
        ui_confirm("Restore built-in stations", "Replace your station list with the built-in stations?",
                   "Restore", UI_C_ERR, reset_ok, NULL);
    }
}

static void more_cb(lv_event_t *e)
{
    (void)e;
    static char item0[64];
    snprintf(item0, sizeof(item0), LV_SYMBOL_POWER "  Resume after restart: %s", g_settings.radio_resume ? "On" : "Off");
    static const char *items[2];
    items[0] = item0;
    items[1] = LV_SYMBOL_REFRESH "  Restore built-in stations";
    ui_menu("Radio options", items, 2, more_pick, NULL);
}

/* ------------------------------------------------------------------ app */

static void radio_create(lv_obj_t *parent)
{
    s_gen++;
    lv_obj_set_flex_flow(parent, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_all(parent, 16, 0);
    lv_obj_set_style_pad_row(parent, 12, 0);
    lv_obj_remove_flag(parent, LV_OBJ_FLAG_SCROLLABLE);

    build_now_playing(parent);

    lv_obj_t *hdr = lv_obj_create(parent);
    lv_obj_remove_style_all(hdr);
    lv_obj_set_size(hdr, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(hdr, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(hdr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(hdr, 10, 0);
    s_count_lbl = ui_section_title(hdr, "STATIONS");
    lv_obj_set_flex_grow(s_count_lbl, 1);
    lv_obj_t *sb = ui_button(hdr, LV_SYMBOL_LIST "  Search", UI_C_SURFACE2);
    lv_obj_set_height(sb, 52);
    lv_obj_add_event_cb(sb, search_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *ab = ui_button(hdr, LV_SYMBOL_PLUS "  Add", UI_C_SURFACE2);
    lv_obj_set_height(ab, 52);
    lv_obj_add_event_cb(ab, add_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *mb = ui_button(hdr, LV_SYMBOL_SETTINGS, UI_C_SURFACE2);
    lv_obj_set_size(mb, 60, 52);
    lv_obj_set_style_pad_hor(mb, 0, 0);
    lv_obj_add_event_cb(mb, more_cb, LV_EVENT_CLICKED, NULL);

    s_list = lv_obj_create(parent);
    lv_obj_remove_style_all(s_list);
    lv_obj_set_width(s_list, lv_pct(100));
    lv_obj_set_flex_grow(s_list, 1);
    lv_obj_set_flex_flow(s_list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_list, 8, 0);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_ACTIVE);

    s_last_state = (radio_state_t)-1;
    s_last_title[0] = 0;
    rebuild_rows();
    s_timer = lv_timer_create(tick_cb, 500, NULL);
}

static void radio_destroy(void)
{
    if (s_timer) lv_timer_delete(s_timer);
    s_timer = NULL;
    results_close();
    s_name = s_title = s_info = s_chip = s_play_lbl = s_vol = s_save_btn = s_out_btn = NULL;
    s_list = s_count_lbl = NULL;
    memset(s_rows, 0, sizeof(s_rows));
    s_gen++;
}

static void radio_on_settings(void)
{
    /* station list or volume may have changed on the web page */
    if (s_vol) lv_slider_set_value(s_vol, g_settings.radio_volume, LV_ANIM_OFF);
    rebuild_rows();
}

const ui_app_t app_radio = {
    .name = "Radio",
    .icon = &icon_radio,
    .create = radio_create,
    .destroy = radio_destroy,
    .on_settings = radio_on_settings,
};

/* simulator hooks */
static void dbg_search_cb(const radio_search_rec_t *r, int n, const char *e, void *u) { search_done(r, n, e, u); }
void app_radio_debug_search(void) { plat_radio_search("toronto", dbg_search_cb, (void *)(uintptr_t)s_gen); }
void app_radio_debug_menu(int i)
{
    s_menu_idx = i;
    static const char *const items[] = {LV_SYMBOL_PLAY "  Play", LV_SYMBOL_EDIT "  Rename",
                                        LV_SYMBOL_SHUFFLE "  Change address", LV_SYMBOL_UP "  Move up",
                                        LV_SYMBOL_DOWN "  Move down", LV_SYMBOL_TRASH "  Delete"};
    ui_menu(g_stations.items[i].name, items, 6, row_menu_pick, NULL);
}
