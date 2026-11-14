/*
 * Web settings server (port 80).
 *   GET  /                 settings page
 *   GET  /api/status       network / time / system status
 *   GET  /api/settings     current settings (Wi-Fi password never returned)
 *   POST /api/settings     partial update (JSON object)
 *   GET  /api/tz           time-zone list
 *   GET  /api/wifi/scan    nearby networks
 *   POST /api/wifi         {"ssid":..,"pass":..}
 *   POST /api/wifi/forget
 *   POST /api/portal       {"on":true|false}
 *   POST /api/reboot, /api/factory_reset
 *   GET  /api/radio        player status, volume and station list
 *   POST /api/radio        {"action":"play","index":n} | {"action":"play_url","name":..,"url":..}
 *                          | {"action":"stop"} | {"volume":n} | {"resume":bool} | {"out_usb":bool}
 *   POST /api/stations     replace the station list (JSON array of {name,url})
 *   POST /api/ota          raw application image (P4Box.ino.bin)
 *   GET  /api/photos?dir=  microSD status and one folder's sub-folders and JPEG files
 *   GET  /api/photo?path=  a photo file (auth may also be passed as &auth=token for <img> / links)
 *   POST /api/photos/upload?dir=..&name=..   raw JPEG body (max 20 MB)
 *   POST /api/photos/delete {"path":..}      file, or empty folder
 *   POST /api/photos/mkdir  {"path":..}
 *   POST /api/c6_ota       raw ESP-Hosted firmware for the ESP32-C6 Wi-Fi module (esp32c6-v2.x.y.bin)
 *   anything else          redirect to / while the setup hotspot is active (captive portal)
 */
#include "web.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "../../drivers/esp_lvgl_port/esp_lvgl_port.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "net.h"
#include "../core/platform.h"
#include "../core/settings.h"
#include "../core/tz_table.h"
#include "../core/stations.h"
#include "../core/lockcore.h"
#include "esp_random.h"
#include "../ui/ui.h"
#include "weather_net.h"
#include "esp_heap_caps.h"
#include "sdcard.h"
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <sys/stat.h>
#include <unistd.h>
#include "esp_heap_caps.h"

static const char *TAG = "web";
static httpd_handle_t s_srv;

#include "../web/index_html.h"

/* ------------------------------------------------------------------ helpers */

static esp_err_t send_json(httpd_req_t *req, cJSON *j)
{
    char *s = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return r;
}

static esp_err_t send_ok(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"ok\":true}");
}

/* ------------------------------------------------------------------ auth
 * Until the lock admin code exists the page is open (first-time setup).  After that every
 * endpoint except the page itself, /api/status, /api/auth and /api/login needs the token
 * returned by /api/login, sent in the X-Auth header.  Sessions expire after 30 idle minutes. */

static char s_token[33];
static uint32_t s_token_exp;

static uint32_t now_s(void) { return (uint32_t)(esp_timer_get_time() / 1000000); }

static bool authed(httpd_req_t *req)
{
    if (!g_lock.admin_set) return true;
    char hdr[40] = "";
    if (!s_token[0] || httpd_req_get_hdr_value_str(req, "X-Auth", hdr, sizeof(hdr)) != ESP_OK) return false;
    if (strcmp(hdr, s_token) != 0 || now_s() > s_token_exp) return false;
    s_token_exp = now_s() + 1800;
    return true;
}

static void url_decode(const char *in, char *out, size_t n)
{
    size_t o = 0;
    for (; *in && o + 1 < n; in++) {
        if (*in == '+') out[o++] = ' ';
        else if (*in == '%' && isxdigit((unsigned char)in[1]) && isxdigit((unsigned char)in[2])) {
            char h[3] = {in[1], in[2], 0};
            out[o++] = (char)strtol(h, NULL, 16);
            in += 2;
        } else out[o++] = *in;
    }
    out[o] = 0;
}

/* Reads and URL-decodes one query parameter. */
static bool query_param(httpd_req_t *req, const char *key, char *out, size_t n)
{
    out[0] = 0;
    size_t ql = httpd_req_get_url_query_len(req);
    if (ql == 0 || ql > 1024) return false;
    char *q = malloc(ql + 1);
    char *raw = malloc(n * 3 + 1);
    bool ok = q && raw && httpd_req_get_url_query_str(req, q, ql + 1) == ESP_OK &&
              httpd_query_key_value(q, key, raw, n * 3 + 1) == ESP_OK;
    if (ok) url_decode(raw, out, n);
    free(q);
    free(raw);
    return ok;
}

static bool authed_query(httpd_req_t *req)
{
    if (authed(req)) return true;
    char tok[40];
    if (!s_token[0] || !query_param(req, "auth", tok, sizeof(tok))) return false;
    if (strcmp(tok, s_token) != 0 || now_s() > s_token_exp) return false;
    s_token_exp = now_s() + 1800;
    return true;
}

static esp_err_t send_401(httpd_req_t *req)
{
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, "{\"auth\":\"required\"}");
}

#define REQUIRE_AUTH(req) do { if (!authed(req)) return send_401(req); } while (0)

static char *read_body(httpd_req_t *req, size_t max)
{
    if (req->content_len == 0 || req->content_len > max) return NULL;
    char *buf = malloc(req->content_len + 1);
    if (!buf) return NULL;
    size_t got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, buf + got, req->content_len - got);
        if (r <= 0) {
            if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
            free(buf);
            return NULL;
        }
        got += r;
    }
    buf[got] = 0;
    return buf;
}

/* ------------------------------------------------------------------ pages */

static esp_err_t h_index(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, index_html, sizeof(index_html) - 1);
}

static esp_err_t h_status(httpd_req_t *req)
{
    net_status_t st;
    net_get_status(&st);
    cJSON *j = cJSON_CreateObject();
    cJSON *eth = cJSON_AddObjectToObject(j, "eth");
    cJSON_AddBoolToObject(eth, "link", st.eth_link);
    cJSON_AddStringToObject(eth, "ip", st.eth_ip);
    cJSON *w = cJSON_AddObjectToObject(j, "wifi");
    cJSON_AddBoolToObject(w, "configured", st.wifi_configured);
    cJSON_AddBoolToObject(w, "connected", st.wifi_connected);
    cJSON_AddBoolToObject(w, "connecting", st.wifi_connecting);
    cJSON_AddStringToObject(w, "ssid", st.wifi_ssid);
    cJSON_AddStringToObject(w, "ip", st.wifi_ip);
    cJSON_AddNumberToObject(w, "rssi", st.wifi_rssi);
    cJSON *p = cJSON_AddObjectToObject(j, "portal");
    cJSON_AddBoolToObject(p, "active", st.portal_active);
    cJSON_AddStringToObject(p, "ssid", st.portal_ssid);
    cJSON_AddStringToObject(j, "active", st.active == NET_ACTIVE_ETH ? "ethernet" : st.active == NET_ACTIVE_WIFI ? "wifi" : "none");
    cJSON_AddBoolToObject(j, "time_synced", st.time_synced);
    {
        char rtc[320];
        plat_rtc_status(rtc, sizeof(rtc));
        cJSON *r = cJSON_AddObjectToObject(j, "rtc");
        cJSON_AddStringToObject(r, "status", rtc);
        cJSON_AddBoolToObject(r, "restored", plat_rtc_restored());
    }
    cJSON_AddNumberToObject(j, "epoch", (double)time(NULL));
    time_t now = time(NULL);
    struct tm t;
    localtime_r(&now, &t);
    char buf[64];
    strftime(buf, sizeof(buf), "%Y-%m-%d %H:%M:%S %Z", &t);
    cJSON_AddStringToObject(j, "local_time", buf);
    cJSON_AddStringToObject(j, "firmware", plat_fw_version());
    cJSON_AddStringToObject(j, "c6_firmware", net_c6_version());
    {
        c6fw_t f;
        plat_c6_fw_status(&f);
        cJSON_AddStringToObject(j, "c6_expected", f.expected);
        cJSON_AddBoolToObject(j, "c6_known", f.known);
        cJSON_AddStringToObject(j, "c6_msg", f.msg);
        cJSON_AddNumberToObject(j, "c6_pct", f.running ? f.pct : -1);
    }
    {
        bool ringing;
        char label[32], next[48];
        lvgl_port_lock(0);
        ui_alarm_status(&ringing, label, sizeof(label), next, sizeof(next));
        lvgl_port_unlock();
        cJSON *a = cJSON_AddObjectToObject(j, "alarm");
        cJSON_AddBoolToObject(a, "ringing", ringing);
        cJSON_AddStringToObject(a, "label", label);
        cJSON_AddStringToObject(a, "next", next);
    }
    char info[512];
    plat_sys_info(info, sizeof(info));
    cJSON_AddStringToObject(j, "info", info);
    cJSON_AddNumberToObject(j, "uptime_s", (double)(esp_timer_get_time() / 1000000));
    return send_json(req, j);
}

static esp_err_t h_settings_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    lvgl_port_lock(0);
    char *s = settings_to_json(&g_settings);
    lvgl_port_unlock();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t r = httpd_resp_sendstr(req, s ? s : "{}");
    free(s);
    return r;
}

static esp_err_t h_settings_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *body = read_body(req, 4096);
    if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    char old_host[sizeof(g_settings.hostname)];
    bool wifi_changed = false;

    lvgl_port_lock(0);
    snprintf(old_host, sizeof(old_host), "%s", g_settings.hostname);
    char old_key[sizeof(g_settings.wx_key)];
    snprintf(old_key, sizeof(old_key), "%s", g_settings.wx_key);
    wx_loc_t old_loc = g_settings.wx_locs[g_settings.wx_loc_sel];
    uint8_t old_count = g_settings.wx_loc_count;
    bool changed = settings_apply_json(&g_settings, body, &wifi_changed);
    bool key_changed = strcmp(old_key, g_settings.wx_key) != 0;
    bool loc_changed = old_count != g_settings.wx_loc_count ||
                       memcmp(&old_loc, &g_settings.wx_locs[g_settings.wx_loc_sel], sizeof(old_loc)) != 0;
    char ssid[33], pass[65];
    snprintf(ssid, sizeof(ssid), "%s", g_settings.wifi_ssid);
    snprintf(pass, sizeof(pass), "%s", g_settings.wifi_pass);
    if (changed) {
        plat_settings_save();
        ui_on_external_settings_change();
    }
    lvgl_port_unlock();
    free(body);

    if (strcmp(old_host, g_settings.hostname) != 0) net_hostname_changed();
    if (wifi_changed) net_wifi_connect(ssid, pass);
    if (key_changed) plat_weather_key_changed();
    else if (loc_changed) plat_weather_refresh(false);
    return h_settings_get(req);
}

static esp_err_t h_tz(httpd_req_t *req)
{
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < g_tz_count; i++) {
        const tz_entry_t *z = &g_tz_table[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", z->id);
        cJSON_AddStringToObject(o, "region", z->region);
        cJSON_AddStringToObject(o, "city", z->city);
        cJSON_AddBoolToObject(o, "dst", z->dst_abbr != NULL);
        if (z->note) cJSON_AddStringToObject(o, "note", z->note);
        cJSON_AddItemToArray(arr, o);
    }
    return send_json(req, arr);
}

static esp_err_t h_scan(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    wifi_scan_rec_t *recs = calloc(24, sizeof(wifi_scan_rec_t));
    int n = recs ? net_wifi_scan(recs, 24) : 0;
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "ssid", recs[i].ssid);
        cJSON_AddNumberToObject(o, "rssi", recs[i].rssi);
        cJSON_AddBoolToObject(o, "secure", recs[i].secure);
        cJSON_AddItemToArray(arr, o);
    }
    free(recs);
    return send_json(req, arr);
}

static esp_err_t h_wifi(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *body = read_body(req, 512);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    cJSON *ss = j ? cJSON_GetObjectItem(j, "ssid") : NULL;
    cJSON *pw = j ? cJSON_GetObjectItem(j, "pass") : NULL;
    if (!cJSON_IsString(ss) || !ss->valuestring[0]) {
        cJSON_Delete(j);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    }
    lvgl_port_lock(0);
    plat_wifi_connect(ss->valuestring, cJSON_IsString(pw) ? pw->valuestring : "");
    lvgl_port_unlock();
    cJSON_Delete(j);
    return send_ok(req);
}

static esp_err_t h_wifi_forget(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    lvgl_port_lock(0);
    plat_wifi_forget();
    lvgl_port_unlock();
    return send_ok(req);
}

static esp_err_t h_portal(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *body = read_body(req, 128);
    bool on = body && strstr(body, "true");
    free(body);
    if (on) net_portal_start(false);
    else net_portal_stop();
    return send_ok(req);
}

static esp_err_t h_reboot(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    send_ok(req);
    plat_reboot();
    return ESP_OK;
}

static esp_err_t h_factory(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    send_ok(req);
    plat_factory_reset();
    return ESP_OK;
}

static const char *state_name(radio_state_t s)
{
    switch (s) {
    case RADIO_CONNECTING: return "connecting";
    case RADIO_BUFFERING: return "buffering";
    case RADIO_PLAYING: return "playing";
    case RADIO_RECONNECTING: return "reconnecting";
    case RADIO_ERROR: return "error";
    default: return "stopped";
    }
}

static esp_err_t h_radio_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    radio_status_t st;
    plat_radio_get_status(&st);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "state", state_name(st.state));
    cJSON_AddStringToObject(j, "name", st.name);
    cJSON_AddStringToObject(j, "url", st.url);
    cJSON_AddStringToObject(j, "title", st.title);
    cJSON_AddStringToObject(j, "codec", st.codec);
    cJSON_AddNumberToObject(j, "bitrate", st.bitrate_kbps);
    cJSON_AddNumberToObject(j, "sample_rate", st.sample_rate);
    cJSON_AddNumberToObject(j, "buffer", st.buffer_pct);
    cJSON_AddStringToObject(j, "error", st.error);
    lvgl_port_lock(0);
    cJSON_AddNumberToObject(j, "volume", g_settings.radio_volume);
    cJSON_AddBoolToObject(j, "resume", g_settings.radio_resume);
    cJSON_AddNumberToObject(j, "last", g_settings.radio_last);
    cJSON_AddNumberToObject(j, "playing_index", stations_find_url(&g_stations, st.url));
    char *list = stations_to_json(&g_stations);
    lvgl_port_unlock();
    audio_out_t ao;
    plat_audio_out_get(&ao);
    cJSON *o = cJSON_AddObjectToObject(j, "output");
    cJSON_AddBoolToObject(o, "usb_connected", ao.connected);
    cJSON_AddBoolToObject(o, "usb_active", ao.active);
    cJSON_AddBoolToObject(o, "prefer_usb", ao.prefer_usb);
    cJSON_AddStringToObject(o, "usb_name", ao.name);
    cJSON_AddItemToObject(j, "stations", cJSON_Parse(list ? list : "[]"));
    free(list);
    return send_json(req, j);
}

static esp_err_t h_radio_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *body = read_body(req, 1024);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    cJSON *a = cJSON_GetObjectItem(j, "action");
    cJSON *v = cJSON_GetObjectItem(j, "volume");
    cJSON *rs = cJSON_GetObjectItem(j, "resume");
    cJSON *ou = cJSON_GetObjectItem(j, "out_usb");
    esp_err_t err = ESP_OK;
    lvgl_port_lock(0);
    if (cJSON_IsBool(ou)) plat_audio_out_prefer_usb(cJSON_IsTrue(ou));
    if (cJSON_IsNumber(v)) plat_radio_set_volume(v->valueint);
    if (cJSON_IsBool(rs)) g_settings.radio_resume = cJSON_IsTrue(rs);
    if (cJSON_IsString(a)) {
        if (!strcmp(a->valuestring, "stop")) {
            plat_radio_stop();
        } else if (!strcmp(a->valuestring, "play")) {
            cJSON *i = cJSON_GetObjectItem(j, "index");
            int idx = cJSON_IsNumber(i) ? i->valueint : g_settings.radio_last;
            if (idx >= 0 && idx < g_stations.count) {
                g_settings.radio_last = idx;
                plat_radio_play(g_stations.items[idx].name, g_stations.items[idx].url);
            } else err = ESP_FAIL;
        } else if (!strcmp(a->valuestring, "play_url")) {
            cJSON *n = cJSON_GetObjectItem(j, "name"), *u = cJSON_GetObjectItem(j, "url");
            if (cJSON_IsString(u) && stations_url_valid(u->valuestring))
                plat_radio_play(cJSON_IsString(n) ? n->valuestring : "", u->valuestring);
            else err = ESP_FAIL;
        }
    }
    plat_settings_save();
    ui_on_external_settings_change();
    lvgl_port_unlock();
    cJSON_Delete(j);
    if (err != ESP_OK) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad station");
    return h_radio_get(req);
}

static esp_err_t h_stations_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *body = read_body(req, 32 * 1024);
    if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    stations_t *tmp = malloc(sizeof(stations_t));
    bool ok = tmp && stations_from_json(tmp, body);
    free(body);
    if (!ok) {
        free(tmp);
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "expected a JSON array of {name,url}");
    }
    lvgl_port_lock(0);
    g_stations = *tmp;
    if (g_settings.radio_last >= g_stations.count) g_settings.radio_last = g_stations.count ? 0 : -1;
    plat_stations_save();
    plat_settings_save();
    ui_on_external_settings_change();
    lvgl_port_unlock();
    free(tmp);
    return h_radio_get(req);
}

static esp_err_t h_ota(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no OTA partition");
    if (req->content_len < 64 * 1024 || req->content_len > part->size)
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "file size is not a P4Box application image");

    esp_ota_handle_t h = 0;
    char *buf = malloc(8192);
    if (!buf) return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "no memory");
    size_t left = req->content_len;
    bool first = true;
    esp_err_t err = ESP_OK;
    const char *msg = NULL;
    ESP_LOGI(TAG, "OTA: %u bytes -> %s", (unsigned)left, part->label);
    while (left > 0) {
        int r = httpd_req_recv(req, buf, left < 8192 ? left : 8192);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { err = ESP_FAIL; msg = "upload interrupted"; break; }
        if (first) {
            first = false;
            if ((uint8_t)buf[0] == 0xFF) { err = ESP_FAIL; msg = "This is the full-flash image. Upload p4box_app.bin instead."; break; }
            if ((uint8_t)buf[0] != 0xE9) { err = ESP_FAIL; msg = "Not an ESP32 application image."; break; }
            err = esp_ota_begin(part, OTA_SIZE_UNKNOWN, &h);
            if (err != ESP_OK) { msg = "could not start update"; break; }
        }
        err = esp_ota_write(h, buf, r);
        if (err != ESP_OK) { msg = "flash write failed"; break; }
        left -= r;
    }
    free(buf);
    if (err == ESP_OK) {
        err = esp_ota_end(h);
        if (err == ESP_OK) err = esp_ota_set_boot_partition(part);
        else msg = "image check failed (wrong chip revision or corrupt file?)";
    } else if (h) {
        esp_ota_abort(h);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA failed: %s", msg ? msg : esp_err_to_name(err));
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, msg ? msg : "update failed");
    }
    send_ok(req);
    ESP_LOGI(TAG, "OTA complete, rebooting");
    plat_reboot();
    return ESP_OK;
}

static esp_err_t h_auth(httpd_req_t *req)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "admin_set", g_lock.admin_set);
    cJSON_AddBoolToObject(j, "ok", authed(req));
    return send_json(req, j);
}

static esp_err_t h_login(httpd_req_t *req)
{
    char *body = read_body(req, 256);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    cJSON *c = j ? cJSON_GetObjectItem(j, "code") : NULL;
    uint32_t left = 0;
    int r = -1;
    lvgl_port_lock(0);
    if (lock_locked_out(&left)) r = -2;
    else if (cJSON_IsString(c)) r = lock_check(c->valuestring, true);
    lvgl_port_unlock();
    cJSON_Delete(j);
    if (r == -2) {
        char m[64];
        snprintf(m, sizeof(m), "Too many wrong codes - wait %u s", (unsigned)left);
        httpd_resp_set_status(req, "429 Too Many Requests");
        return httpd_resp_sendstr(req, m);
    }
    if (r != LOCK_ADMIN) {
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "Wrong admin code");
    }
    uint8_t rnd[16];
    esp_fill_random(rnd, sizeof(rnd));
    for (int i = 0; i < 16; i++) sprintf(s_token + i * 2, "%02x", rnd[i]);
    s_token_exp = now_s() + 1800;
    lvgl_port_lock(0);
    lock_log(LEV_ADMIN, "web page");
    lvgl_port_unlock();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "token", s_token);
    return send_json(req, o);
}

static esp_err_t h_logout(httpd_req_t *req)
{
    s_token[0] = 0;
    return send_ok(req);
}

static esp_err_t h_lock_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    cJSON *j = cJSON_CreateObject();
    lvgl_port_lock(0);
    cJSON_AddBoolToObject(j, "admin_set", g_lock.admin_set);
    cJSON *r1 = cJSON_AddObjectToObject(j, "relay1");
    cJSON_AddStringToObject(r1, "name", g_lock.relay1_name);
    cJSON_AddBoolToObject(r1, "unlocked", g_lock.relay1_unlocked);
    cJSON_AddNumberToObject(r1, "mode", g_lock.relay1_mode);
    cJSON_AddNumberToObject(r1, "pulse_s", g_lock.relay1_pulse_s);
    cJSON_AddNumberToObject(r1, "relock_in", lock_relock_seconds_left());
    cJSON *r2 = cJSON_AddObjectToObject(j, "relay2");
    cJSON_AddStringToObject(r2, "name", g_lock.relay2_name);
    cJSON_AddBoolToObject(r2, "on", g_lock.relay2_on);
    cJSON_AddBoolToObject(r2, "needs_pin", g_lock.relay2_needs_pin);
    cJSON_AddBoolToObject(j, "web_control", g_lock.web_control);
    cJSON_AddNumberToObject(j, "max_attempts", g_lock.max_attempts);
    cJSON_AddNumberToObject(j, "lockout_s", g_lock.lockout_s);
    cJSON *us = cJSON_AddArrayToObject(j, "users");
    for (int i = 0; i < LOCK_MAX_USERS; i++) {
        if (!g_lock.users[i].used) continue;
        cJSON *u = cJSON_CreateObject();
        cJSON_AddNumberToObject(u, "idx", i);
        cJSON_AddStringToObject(u, "label", g_lock.users[i].label);
        cJSON_AddItemToArray(us, u);
    }
    cJSON *lg = cJSON_AddArrayToObject(j, "log");
    for (int i = g_lock_log_count - 1, n = 0; i >= 0 && n < 40; i--, n++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "t", (double)g_lock_log[i].t);
        cJSON_AddStringToObject(e, "text", lock_ev_text(g_lock_log[i].type));
        cJSON_AddStringToObject(e, "who", g_lock_log[i].who);
        cJSON_AddItemToArray(lg, e);
    }
    lvgl_port_unlock();
    return send_json(req, j);
}

static const char *lock_post_apply(cJSON *j)
{
    cJSON *a = cJSON_GetObjectItem(j, "action");
    if (cJSON_IsString(a)) {
        const char *s = a->valuestring;
        if (!strcmp(s, "lock")) { lock_lock("web page", false); return NULL; }
        if (!g_lock.web_control) return "Lock control from the web page is turned off (enable it in Lock > Admin)";
        if (!strcmp(s, "unlock")) lock_unlock("web page");
        else if (!strcmp(s, "r2_on")) lock_relay2_set(true, "web page");
        else if (!strcmp(s, "r2_off")) lock_relay2_set(false, "web page");
        return NULL;
    }
    cJSON *c = cJSON_GetObjectItem(j, "config");
    if (cJSON_IsObject(c)) {
        cJSON *v;
        if (cJSON_IsString(v = cJSON_GetObjectItem(c, "relay1_name")) && v->valuestring[0]) snprintf(g_lock.relay1_name, sizeof(g_lock.relay1_name), "%s", v->valuestring);
        if (cJSON_IsString(v = cJSON_GetObjectItem(c, "relay2_name")) && v->valuestring[0]) snprintf(g_lock.relay2_name, sizeof(g_lock.relay2_name), "%s", v->valuestring);
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(c, "relay1_mode"))) {
            g_lock.relay1_mode = v->valueint ? RELAY1_MOMENTARY : RELAY1_LATCHED;
            if (g_lock.relay1_mode == RELAY1_MOMENTARY && g_lock.relay1_unlocked) lock_lock("settings", true);
        }
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(c, "relay1_pulse_s")) && v->valueint >= 1 && v->valueint <= 600) g_lock.relay1_pulse_s = v->valueint;
        if (cJSON_IsBool(v = cJSON_GetObjectItem(c, "relay2_needs_pin"))) g_lock.relay2_needs_pin = cJSON_IsTrue(v);
        if (cJSON_IsBool(v = cJSON_GetObjectItem(c, "web_control"))) g_lock.web_control = cJSON_IsTrue(v);
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(c, "max_attempts")) && v->valueint >= 3 && v->valueint <= 20) g_lock.max_attempts = v->valueint;
        if (cJSON_IsNumber(v = cJSON_GetObjectItem(c, "lockout_s")) && v->valueint >= 10 && v->valueint <= 3600) g_lock.lockout_s = v->valueint;
        plat_lock_save();
        return NULL;
    }
    cJSON *u = cJSON_GetObjectItem(j, "user");
    if (cJSON_IsObject(u)) {
        cJSON *ix = cJSON_GetObjectItem(u, "idx"), *lb = cJSON_GetObjectItem(u, "label"), *cd = cJSON_GetObjectItem(u, "code");
        int idx = cJSON_IsNumber(ix) ? ix->valueint : -1;
        if (idx < 0) for (int i = 0; i < LOCK_MAX_USERS; i++) if (!g_lock.users[i].used) { idx = i; break; }
        if (idx < 0 || idx >= LOCK_MAX_USERS) return "All 8 code slots are in use";
        const char *code = cJSON_IsString(cd) && cd->valuestring[0] ? cd->valuestring : NULL;
        if (!g_lock.users[idx].used && !code) return "A new user needs a code";
        if (code && !lock_code_valid(code, 4, 6)) return "User codes have 4 to 6 digits";
        if (code && lock_code_in_use(code, idx)) return "That code is already in use";
        lock_set_user(idx, cJSON_IsString(lb) ? lb->valuestring : NULL, code);
        return NULL;
    }
    cJSON *d = cJSON_GetObjectItem(j, "delete_user");
    if (cJSON_IsNumber(d)) {
        if (lock_user_count() <= 1) return "Keep at least one user code";
        lock_delete_user(d->valueint);
        return NULL;
    }
    cJSON *ac = cJSON_GetObjectItem(j, "admin_code");
    if (cJSON_IsString(ac)) {
        if (!g_lock.admin_set) return "Create the admin code on the panel first (Lock app)";
        if (!lock_code_valid(ac->valuestring, 6, 6)) return "The admin code has exactly 6 digits";
        if (lock_code_in_use(ac->valuestring, LOCK_ADMIN)) return "That code is already a user code";
        lock_set_admin(ac->valuestring);
        return NULL;
    }
    return "Nothing to do";
}

static esp_err_t h_lock_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    if (!g_lock.admin_set) return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "Create the admin code on the panel first (Lock app)");
    char *body = read_body(req, 1024);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    if (!j) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad json");
    lvgl_port_lock(0);
    const char *err = lock_post_apply(j);
    ui_on_external_settings_change();
    lvgl_port_unlock();
    cJSON_Delete(j);
    if (err) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, err);
    return h_lock_get(req);
}

/* ------------------------------------------------------------------ photos (microSD) */

static esp_err_t send_err(httpd_req_t *req, const char *status, const char *msg)
{
    httpd_resp_set_status(req, status);
    httpd_resp_set_type(req, "text/plain");
    return httpd_resp_sendstr(req, msg);
}

static bool sd_ready_or_err(httpd_req_t *req)
{
    sd_status_t st;
    plat_sd_status(&st);
    if (st.state == SD_READY) return true;
    send_err(req, "503 Service Unavailable", st.state == SD_MOUNTING ? "The card is being read, try again" : "No microSD card");
    return false;
}

static bool jpeg_name_ok(const char *n)
{
    if (!n[0] || strchr(n, '/') || strchr(n, '\\') || n[0] == '.') return false;
    const char *dot = strrchr(n, '.');
    return dot && (!strcasecmp(dot, ".jpg") || !strcasecmp(dot, ".jpeg"));
}

static esp_err_t h_photos_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char dir[200] = "";
    query_param(req, "dir", dir, sizeof(dir));
    sd_status_t st;
    plat_sd_status(&st);
    cJSON *j = cJSON_CreateObject();
    cJSON *sd = cJSON_AddObjectToObject(j, "sd");
    static const char *names[] = {"none", "mounting", "ready", "error"};
    cJSON_AddStringToObject(sd, "state", names[st.state]);
    cJSON_AddNumberToObject(sd, "total_mb", (double)(st.total_bytes / 1048576));
    cJSON_AddNumberToObject(sd, "used_mb", (double)(st.used_bytes / 1048576));
    cJSON_AddStringToObject(sd, "error", st.error);
    cJSON_AddStringToObject(j, "dir", dir);
    cJSON *arr = cJSON_AddArrayToObject(j, "entries");
    char path[300];
    if (st.state == SD_READY && photos_abs_path(dir, path, sizeof(path))) {
        DIR *d = opendir(path);
        if (d) {
            struct dirent *e;
            int n = 0;
            while ((e = readdir(d)) && n < 2000) {
                if (e->d_name[0] == '.' || !strcasecmp(e->d_name, "System Volume Information")) continue;
                char full[400];
                struct stat fs;
                snprintf(full, sizeof(full), "%s/%s", path, e->d_name);
                if (stat(full, &fs) != 0) continue;
                bool isdir = S_ISDIR(fs.st_mode);
                if (!isdir && !jpeg_name_ok(e->d_name)) continue;
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "name", e->d_name);
                cJSON_AddBoolToObject(o, "dir", isdir);
                cJSON_AddNumberToObject(o, "size", isdir ? 0 : (double)fs.st_size);
                cJSON_AddItemToArray(arr, o);
                n++;
            }
            closedir(d);
        } else {
            cJSON_AddStringToObject(j, "error", "Folder not found");
        }
    }
    return send_json(req, j);
}

static esp_err_t h_photo_file(httpd_req_t *req)
{
    if (!authed_query(req)) return send_401(req);
    if (!sd_ready_or_err(req)) return ESP_OK;
    char rel[300], path[340];
    if (!query_param(req, "path", rel, sizeof(rel)) || !photos_abs_path(rel, path, sizeof(path)))
        return send_err(req, "400 Bad Request", "bad path");
    FILE *f = fopen(path, "rb");
    if (!f) return send_err(req, "404 Not Found", "not found");
    httpd_resp_set_type(req, "image/jpeg");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=300");
    char *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    size_t n;
    esp_err_t r = ESP_OK;
    while (buf && (n = fread(buf, 1, 16384, f)) > 0) {
        if ((r = httpd_resp_send_chunk(req, buf, n)) != ESP_OK) break;
    }
    fclose(f);
    heap_caps_free(buf);
    if (r == ESP_OK) httpd_resp_send_chunk(req, NULL, 0);
    return r;
}

static esp_err_t h_photo_upload(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    if (!sd_ready_or_err(req)) return ESP_OK;
    char dir[200] = "", name[100] = "", rel[300], path[340], tmp[350];
    query_param(req, "dir", dir, sizeof(dir));
    query_param(req, "name", name, sizeof(name));
    if (!jpeg_name_ok(name)) return send_err(req, "400 Bad Request", "Only .jpg / .jpeg files can be uploaded");
    if (req->content_len == 0 || req->content_len > 20 * 1024 * 1024)
        return send_err(req, "413 Payload Too Large", "Photos must be smaller than 20 MB");
    snprintf(rel, sizeof(rel), "%s%s%s", dir, dir[0] ? "/" : "", name);
    if (!photos_abs_path(rel, path, sizeof(path))) return send_err(req, "400 Bad Request", "bad path");
    snprintf(tmp, sizeof(tmp), "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) return send_err(req, "500 Internal Server Error", "Could not create the file (folder missing or card full?)");
    char *buf = heap_caps_malloc(32768, MALLOC_CAP_SPIRAM);
    size_t left = req->content_len;
    bool ok = buf != NULL;
    while (ok && left > 0) {
        int r = httpd_req_recv(req, buf, left < 32768 ? left : 32768);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) { ok = false; break; }
        if (fwrite(buf, 1, r, f) != (size_t)r) { ok = false; break; }
        left -= r;
    }
    heap_caps_free(buf);
    fclose(f);
    if (!ok) {
        unlink(tmp);
        return send_err(req, "500 Internal Server Error", "Upload failed (card full?)");
    }
    unlink(path);
    if (rename(tmp, path) != 0) {
        unlink(tmp);
        return send_err(req, "500 Internal Server Error", "Could not save the file");
    }
    ESP_LOGI(TAG, "photo uploaded: %s (%u bytes)", rel, (unsigned)req->content_len);
    return send_ok(req);
}

static esp_err_t h_photo_delete(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    if (!sd_ready_or_err(req)) return ESP_OK;
    char *body = read_body(req, 1024);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    cJSON *p = j ? cJSON_GetObjectItem(j, "path") : NULL;
    char path[340];
    esp_err_t r;
    if (!cJSON_IsString(p) || !p->valuestring[0] || !photos_abs_path(p->valuestring, path, sizeof(path))) {
        r = send_err(req, "400 Bad Request", "bad path");
    } else {
        struct stat st = {0};
        int rc = -1;
        if (stat(path, &st) == 0) rc = S_ISDIR(st.st_mode) ? rmdir(path) : unlink(path);
        r = rc == 0 ? send_ok(req) : send_err(req, "409 Conflict", S_ISDIR(st.st_mode) ? "Folder is not empty" : "Could not delete");
    }
    cJSON_Delete(j);
    return r;
}

static esp_err_t h_photo_mkdir(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    if (!sd_ready_or_err(req)) return ESP_OK;
    char *body = read_body(req, 1024);
    cJSON *j = body ? cJSON_Parse(body) : NULL;
    free(body);
    cJSON *p = j ? cJSON_GetObjectItem(j, "path") : NULL;
    char path[340];
    esp_err_t r;
    if (!cJSON_IsString(p) || !p->valuestring[0] || strchr(p->valuestring, '\\') || !photos_abs_path(p->valuestring, path, sizeof(path)))
        r = send_err(req, "400 Bad Request", "bad name");
    else
        r = (mkdir(path, 0775) == 0 || errno == EEXIST) ? send_ok(req) : send_err(req, "500 Internal Server Error", "Could not create the folder");
    cJSON_Delete(j);
    return r;
}

static esp_err_t h_c6_ota(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    if (req->content_len < 100000 || req->content_len > 4 * 1024 * 1024)
        return send_err(req, "400 Bad Request", "That does not look like an ESP-Hosted firmware file");
    uint8_t *buf = heap_caps_malloc(8192, MALLOC_CAP_SPIRAM);
    if (!buf) return send_err(req, "500 Internal Server Error", "out of memory");
    if (!net_c6_ota_begin()) {
        heap_caps_free(buf);
        net_c6_ota_result(false, net_c6_ota_error());
        return send_err(req, "503 Service Unavailable", net_c6_ota_error());
    }
    size_t left = req->content_len, done = 0;
    bool ok = true;
    const char *why = NULL;
    int timeouts = 0;
    while (left > 0) {
        /* net_c6_ota_write() cuts this into single-frame pieces for the C6 */
        int r = httpd_req_recv(req, (char *)buf, left < 8192 ? left : 8192);
        if (r == HTTPD_SOCK_ERR_TIMEOUT && ++timeouts < 10) continue;
        if (r <= 0) { ok = false; why = "Upload interrupted"; net_c6_ota_abort(); break; }
        timeouts = 0;
        if (!net_c6_ota_write(buf, r)) { ok = false; why = net_c6_ota_error(); break; }
        left -= r;
        done += r;
        net_c6_ota_progress((int)(done * 100 / req->content_len), "Writing to the Wi-Fi module...");
    }
    heap_caps_free(buf);
    if (ok) {
        net_c6_ota_progress(100, "Checking the new firmware...");
        ok = net_c6_ota_finish();
        if (!ok) why = net_c6_ota_error();
    }
    net_c6_ota_result(ok, ok ? "Done - restarting..." : why);
    if (!ok) return send_err(req, "500 Internal Server Error", why);
    send_ok(req);
    ESP_LOGI(TAG, "C6 firmware updated from the web page, restarting");
    plat_reboot();
    return ESP_OK;
}

/* GET /api/weather: what the panel shows for the selected place (metric) */
static esp_err_t h_weather_get(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    weather_t *w = heap_caps_calloc(1, sizeof(weather_t), MALLOC_CAP_SPIRAM);
    if (!w) return send_err(req, "500 Internal Server Error", "out of memory");
    lvgl_port_lock(0);
    bool has = g_settings.wx_loc_count > 0;
    wx_loc_t loc = g_settings.wx_locs[g_settings.wx_loc_sel];
    lvgl_port_unlock();
    if (has) plat_weather_get(&loc, w);
    cJSON *j = cJSON_CreateObject();
    cJSON_AddBoolToObject(j, "valid", w->valid && w->temp != WX_NA);
    cJSON_AddStringToObject(j, "error", w->error);
    cJSON_AddStringToObject(j, "place", has ? loc.name : "");
    cJSON_AddStringToObject(j, "onecall", plat_weather_onecall());
    cJSON_AddStringToObject(j, "source", w->source == WX_SRC_ONECALL4 ? "One Call 4.0" : w->source == WX_SRC_ONECALL3 ? "One Call 3.0" : "free plan");
    cJSON_AddNumberToObject(j, "fetched", (double)w->fetched);
    if (w->valid && w->temp != WX_NA) {
        cJSON_AddNumberToObject(j, "temp_c", w->temp);
        cJSON_AddNumberToObject(j, "feels_c", w->feels);
        cJSON_AddNumberToObject(j, "humidity", w->humidity);
        cJSON_AddNumberToObject(j, "dew_point_c", w->dew_point);
        cJSON_AddNumberToObject(j, "pressure_hpa", w->pressure);
        cJSON_AddNumberToObject(j, "wind_ms", w->wind_ms);
        if (w->gust_ms != WX_NA) cJSON_AddNumberToObject(j, "gust_ms", w->gust_ms);
        cJSON_AddNumberToObject(j, "wind_deg", w->wind_deg);
        cJSON_AddStringToObject(j, "description", w->desc);
        cJSON_AddStringToObject(j, "icon", w->icon);
        if (w->uvi != WX_NA) cJSON_AddNumberToObject(j, "uvi", w->uvi);
        cJSON_AddNumberToObject(j, "sunrise", (double)w->sunrise);
        cJSON_AddNumberToObject(j, "sunset", (double)w->sunset);
        if (w->n_days) {
            cJSON_AddNumberToObject(j, "high_c", w->days[0].tmax);
            cJSON_AddNumberToObject(j, "low_c", w->days[0].tmin);
            cJSON_AddNumberToObject(j, "pop", w->days[0].pop);
        }
        float kmh = w->wind_ms * 3.6f;
        if (wx_wind_chill_applies(w->temp, kmh)) cJSON_AddNumberToObject(j, "wind_chill_c", wx_wind_chill(w->temp, kmh));
        if (w->dew_point != WX_NA) {
            float h = wx_humidex(w->temp, w->dew_point);
            if (wx_humidex_applies(w->temp, h)) cJSON_AddNumberToObject(j, "humidex", h);
        }
        if (w->aq_valid) {
            cJSON_AddNumberToObject(j, "aqi_owm", w->owm_aqi);
            cJSON_AddNumberToObject(j, "aqhi", wx_aqhi(w->o3, w->no2, w->pm2_5));
            cJSON_AddNumberToObject(j, "us_aqi", wx_us_aqi(w->pm2_5, w->pm10, w->o3));
        }
        if (w->alert_event[0]) cJSON_AddStringToObject(j, "alert", w->alert_event);
    }
    heap_caps_free(w);
    return send_json(req, j);
}

/* GET /api/weather/search?q=Barrie: place search through the panel (the key stays on the panel) */
static esp_err_t h_weather_search(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char q[64] = "";
    query_param(req, "q", q, sizeof(q));
    if (!q[0]) return send_err(req, "400 Bad Request", "missing q");
    wx_loc_t r[8];
    char err[96] = "";
    int n = weather_search_sync(q, r, 8, err, sizeof(err));
    if (n <= 0) return send_err(req, n == 0 ? "404 Not Found" : "502 Bad Gateway", err[0] ? err : "No place found");
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", r[i].name);
        cJSON_AddStringToObject(o, "region", r[i].region);
        cJSON_AddStringToObject(o, "country", r[i].country);
        cJSON_AddNumberToObject(o, "lat", r[i].lat);
        cJSON_AddNumberToObject(o, "lon", r[i].lon);
        cJSON_AddItemToArray(a, o);
    }
    return send_json(req, a);
}

/* POST {"action":"stop"|"snooze"}: answer a ringing alarm from the web page */
static esp_err_t h_alarm_post(httpd_req_t *req)
{
    REQUIRE_AUTH(req);
    char *body = read_body(req, 256);
    if (!body) return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad body");
    cJSON *j = cJSON_Parse(body);
    free(body);
    cJSON *a = cJSON_GetObjectItem(j, "action");
    const char *act = cJSON_IsString(a) ? a->valuestring : "";
    lvgl_port_lock(0);
    if (!strcmp(act, "snooze")) ui_alarm_snooze();
    else if (!strcmp(act, "stop")) ui_alarm_stop();
    lvgl_port_unlock();
    cJSON_Delete(j);
    return send_ok(req);
}

static esp_err_t h_fallback(httpd_req_t *req)
{
    net_status_t st;
    net_get_status(&st);
    if (st.portal_active) {
        /* Captive-portal probes (generate_204, hotspot-detect.html, connecttest.txt, ...) */
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        return httpd_resp_send(req, NULL, 0);
    }
    return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "not found");
}

/* ------------------------------------------------------------------ start */

void web_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 48;
    cfg.stack_size = 10240;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    cfg.lru_purge_enable = true;
    cfg.max_open_sockets = 7;
    cfg.recv_wait_timeout = 15;
    cfg.send_wait_timeout = 15;
    if (httpd_start(&s_srv, &cfg) != ESP_OK) {
        ESP_LOGE(TAG, "httpd start failed");
        return;
    }
    const httpd_uri_t uris[] = {
        {.uri = "/", .method = HTTP_GET, .handler = h_index},
        {.uri = "/index.html", .method = HTTP_GET, .handler = h_index},
        {.uri = "/api/status", .method = HTTP_GET, .handler = h_status},
        {.uri = "/api/settings", .method = HTTP_GET, .handler = h_settings_get},
        {.uri = "/api/settings", .method = HTTP_POST, .handler = h_settings_post},
        {.uri = "/api/tz", .method = HTTP_GET, .handler = h_tz},
        {.uri = "/api/wifi/scan", .method = HTTP_GET, .handler = h_scan},
        {.uri = "/api/wifi", .method = HTTP_POST, .handler = h_wifi},
        {.uri = "/api/wifi/forget", .method = HTTP_POST, .handler = h_wifi_forget},
        {.uri = "/api/portal", .method = HTTP_POST, .handler = h_portal},
        {.uri = "/api/reboot", .method = HTTP_POST, .handler = h_reboot},
        {.uri = "/api/factory_reset", .method = HTTP_POST, .handler = h_factory},
        {.uri = "/api/ota", .method = HTTP_POST, .handler = h_ota},
        {.uri = "/api/radio", .method = HTTP_GET, .handler = h_radio_get},
        {.uri = "/api/radio", .method = HTTP_POST, .handler = h_radio_post},
        {.uri = "/api/stations", .method = HTTP_POST, .handler = h_stations_post},
        {.uri = "/api/auth", .method = HTTP_GET, .handler = h_auth},
        {.uri = "/api/login", .method = HTTP_POST, .handler = h_login},
        {.uri = "/api/logout", .method = HTTP_POST, .handler = h_logout},
        {.uri = "/api/lock", .method = HTTP_GET, .handler = h_lock_get},
        {.uri = "/api/lock", .method = HTTP_POST, .handler = h_lock_post},
        {.uri = "/api/photos", .method = HTTP_GET, .handler = h_photos_get},
        {.uri = "/api/photo", .method = HTTP_GET, .handler = h_photo_file},
        {.uri = "/api/photos/upload", .method = HTTP_POST, .handler = h_photo_upload},
        {.uri = "/api/photos/delete", .method = HTTP_POST, .handler = h_photo_delete},
        {.uri = "/api/photos/mkdir", .method = HTTP_POST, .handler = h_photo_mkdir},
        {.uri = "/api/c6_ota", .method = HTTP_POST, .handler = h_c6_ota},
        {.uri = "/api/alarm", .method = HTTP_POST, .handler = h_alarm_post},
        {.uri = "/api/weather", .method = HTTP_GET, .handler = h_weather_get},
        {.uri = "/api/weather/search", .method = HTTP_GET, .handler = h_weather_search},
        {.uri = "/*", .method = HTTP_GET, .handler = h_fallback},
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) httpd_register_uri_handler(s_srv, &uris[i]);
    ESP_LOGI(TAG, "web server on port 80");
}
