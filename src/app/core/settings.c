#include "settings.h"
#include "tz_table.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

settings_t g_settings;

void settings_set_defaults(settings_t *s)
{
    memset(s, 0, sizeof(*s));
    s->version = SETTINGS_VERSION;
    snprintf(s->hostname, sizeof(s->hostname), "p4box");
    s->brightness = 80;
    s->screensaver_s = 120;
    s->night_dim = true;
    s->night_start_h = 22;
    s->night_end_h = 7;
    s->night_brightness = 25;
    snprintf(s->tz_id, sizeof(s->tz_id), "ca_toronto");
    s->dst_mode = DST_AUTO;
    s->clock_24h = false;
    s->clock_seconds = true;
    s->clock_face = CLOCK_FACE_DIGITAL;
    snprintf(s->ntp_server, sizeof(s->ntp_server), "pool.ntp.org");
    snprintf(s->province, sizeof(s->province), "ON");
    s->radio_volume = 60;
    s->radio_last = 0;
    s->radio_resume = false;
    s->saver_mode = SAVER_CLOCK;
    s->photo_interval_s = 10;
    s->photo_shuffle = false;
    s->photo_fill = false;
    s->photo_recursive = true;
    s->photo_frame_clock = true;
    s->photo_dir[0] = 0;
    s->alarm_snooze_min = 9;
    s->alarm_volume = 70;
    s->alarm_fade = true;
    s->alarm_ring_min = 30;
    s->wx_units = WX_METRIC;
    s->wx_aq_scale = WX_AQ_AQHI;
    s->wx_home = true;
    s->radio_out_usb = true;
    s->rtc_vbat = true;
}

static uint8_t clamp_u8(int v, int lo, int hi) { return (uint8_t)(v < lo ? lo : v > hi ? hi : v); }

void settings_sanitize(settings_t *s)
{
    s->version = SETTINGS_VERSION;
    s->wifi_ssid[sizeof(s->wifi_ssid) - 1] = 0;
    s->wifi_pass[sizeof(s->wifi_pass) - 1] = 0;
    s->hostname[sizeof(s->hostname) - 1] = 0;
    s->tz_id[sizeof(s->tz_id) - 1] = 0;
    s->tz_custom[sizeof(s->tz_custom) - 1] = 0;
    s->ntp_server[sizeof(s->ntp_server) - 1] = 0;
    s->province[sizeof(s->province) - 1] = 0;
    if (!s->hostname[0]) snprintf(s->hostname, sizeof(s->hostname), "p4box");
    s->brightness = clamp_u8(s->brightness, 5, 100);
    s->night_brightness = clamp_u8(s->night_brightness, 1, 100);
    s->night_start_h = clamp_u8(s->night_start_h, 0, 23);
    s->night_end_h = clamp_u8(s->night_end_h, 0, 23);
    if (s->screensaver_s > 3600) s->screensaver_s = 3600;
    if (s->dst_mode > DST_ALWAYS_DAYLIGHT) s->dst_mode = DST_AUTO;
    if (s->clock_face > CLOCK_FACE_ANALOG) s->clock_face = CLOCK_FACE_DIGITAL;
    if (!tz_find(s->tz_id)) snprintf(s->tz_id, sizeof(s->tz_id), "ca_toronto");
    if (!s->ntp_server[0]) snprintf(s->ntp_server, sizeof(s->ntp_server), "pool.ntp.org");
    if (!s->province[0]) snprintf(s->province, sizeof(s->province), "ON");
    if (s->radio_volume > 100) s->radio_volume = 100;
    if (s->radio_last < -1) s->radio_last = -1;
    if (s->saver_mode > SAVER_PHOTOS) s->saver_mode = SAVER_CLOCK;
    if (s->photo_interval_s < 3) s->photo_interval_s = 3;
    if (s->photo_interval_s > 250) s->photo_interval_s = 250;
    s->photo_dir[sizeof(s->photo_dir) - 1] = 0;
    if (strstr(s->photo_dir, "..")) s->photo_dir[0] = 0;
    while (s->photo_dir[0] == '/') memmove(s->photo_dir, s->photo_dir + 1, strlen(s->photo_dir));
    for (int i = 0; i < ALARM_MAX; i++) {
        alarm_t *a = &s->alarms[i];
        a->hour = clamp_u8(a->hour, 0, 23);
        a->minute = clamp_u8(a->minute, 0, 59);
        a->days &= 0x7F;
        if (a->sound > ALARM_SND_RADIO) a->sound = ALARM_SND_BEEP;
        a->label[sizeof(a->label) - 1] = 0;
        if (!a->used) a->on = false;
    }
    s->alarm_snooze_min = clamp_u8(s->alarm_snooze_min, 1, 10);
    s->alarm_volume = clamp_u8(s->alarm_volume, 10, 100);
    s->alarm_ring_min = clamp_u8(s->alarm_ring_min, 5, 60);
    s->last_play_name[sizeof(s->last_play_name) - 1] = 0;
    s->last_play_url[sizeof(s->last_play_url) - 1] = 0;
    s->wx_key[sizeof(s->wx_key) - 1] = 0;
    for (char *p = s->wx_key; *p; p++) if (*p == ' ') memmove(p, p + 1, strlen(p));
    if (s->wx_units > WX_IMPERIAL) s->wx_units = WX_METRIC;
    if (s->wx_aq_scale > WX_AQ_OWM) s->wx_aq_scale = WX_AQ_AQHI;
    if (s->wx_loc_count > WX_LOC_MAX) s->wx_loc_count = 0;
    for (int i = 0; i < WX_LOC_MAX; i++) {
        wx_loc_t *l = &s->wx_locs[i];
        l->name[sizeof(l->name) - 1] = 0;
        l->region[sizeof(l->region) - 1] = 0;
        l->country[sizeof(l->country) - 1] = 0;
        if (!(l->lat >= -90 && l->lat <= 90 && l->lon >= -180 && l->lon <= 180)) l->lat = l->lon = 0;
    }
    if (s->wx_loc_sel >= s->wx_loc_count) s->wx_loc_sel = 0;
}

char *settings_to_json(const settings_t *s)
{
    cJSON *j = cJSON_CreateObject();
    cJSON_AddStringToObject(j, "wifi_ssid", s->wifi_ssid);
    cJSON_AddBoolToObject(j, "wifi_pass_set", s->wifi_pass[0] != 0);
    cJSON_AddStringToObject(j, "hostname", s->hostname);
    cJSON_AddNumberToObject(j, "brightness", s->brightness);
    cJSON_AddNumberToObject(j, "screensaver_s", s->screensaver_s);
    cJSON_AddBoolToObject(j, "night_dim", s->night_dim);
    cJSON_AddNumberToObject(j, "night_start_h", s->night_start_h);
    cJSON_AddNumberToObject(j, "night_end_h", s->night_end_h);
    cJSON_AddNumberToObject(j, "night_brightness", s->night_brightness);
    cJSON_AddStringToObject(j, "tz_id", s->tz_id);
    cJSON_AddNumberToObject(j, "dst_mode", s->dst_mode);
    cJSON_AddStringToObject(j, "tz_custom", s->tz_custom);
    cJSON_AddBoolToObject(j, "clock_24h", s->clock_24h);
    cJSON_AddBoolToObject(j, "clock_seconds", s->clock_seconds);
    cJSON_AddNumberToObject(j, "clock_face", s->clock_face);
    cJSON_AddStringToObject(j, "ntp_server", s->ntp_server);
    cJSON_AddStringToObject(j, "province", s->province);
    cJSON_AddNumberToObject(j, "radio_volume", s->radio_volume);
    cJSON_AddBoolToObject(j, "radio_resume", s->radio_resume);
    cJSON_AddBoolToObject(j, "radio_out_usb", s->radio_out_usb);
    cJSON_AddBoolToObject(j, "rtc_vbat", s->rtc_vbat);
    cJSON_AddNumberToObject(j, "saver_mode", s->saver_mode);
    cJSON_AddNumberToObject(j, "photo_interval_s", s->photo_interval_s);
    cJSON_AddBoolToObject(j, "photo_shuffle", s->photo_shuffle);
    cJSON_AddBoolToObject(j, "photo_fill", s->photo_fill);
    cJSON_AddBoolToObject(j, "photo_recursive", s->photo_recursive);
    cJSON_AddBoolToObject(j, "photo_frame_clock", s->photo_frame_clock);
    cJSON_AddStringToObject(j, "photo_dir", s->photo_dir);
    cJSON *al = cJSON_AddArrayToObject(j, "alarms");
    for (int i = 0; i < ALARM_MAX; i++) {
        const alarm_t *a = &s->alarms[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "used", a->used);
        cJSON_AddBoolToObject(o, "on", a->on);
        cJSON_AddNumberToObject(o, "hour", a->hour);
        cJSON_AddNumberToObject(o, "minute", a->minute);
        cJSON_AddNumberToObject(o, "days", a->days);
        cJSON_AddNumberToObject(o, "sound", a->sound);
        cJSON_AddStringToObject(o, "label", a->label);
        cJSON_AddItemToArray(al, o);
    }
    cJSON_AddNumberToObject(j, "alarm_snooze_min", s->alarm_snooze_min);
    cJSON_AddNumberToObject(j, "alarm_volume", s->alarm_volume);
    cJSON_AddBoolToObject(j, "alarm_fade", s->alarm_fade);
    cJSON_AddNumberToObject(j, "alarm_ring_min", s->alarm_ring_min);
    cJSON_AddStringToObject(j, "last_play_name", s->last_play_name);
    /* the key itself is never sent back, only whether one is set and its last characters */
    cJSON_AddBoolToObject(j, "wx_key_set", s->wx_key[0] != 0);
    size_t kl = strlen(s->wx_key);
    cJSON_AddStringToObject(j, "wx_key_tail", kl > 4 ? s->wx_key + kl - 4 : "");
    cJSON_AddNumberToObject(j, "wx_units", s->wx_units);
    cJSON_AddNumberToObject(j, "wx_aq_scale", s->wx_aq_scale);
    cJSON_AddBoolToObject(j, "wx_home", s->wx_home);
    cJSON_AddNumberToObject(j, "wx_loc_sel", s->wx_loc_sel);
    cJSON *wl = cJSON_AddArrayToObject(j, "wx_locs");
    for (int i = 0; i < s->wx_loc_count; i++) {
        const wx_loc_t *l = &s->wx_locs[i];
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", l->name);
        cJSON_AddStringToObject(o, "region", l->region);
        cJSON_AddStringToObject(o, "country", l->country);
        cJSON_AddNumberToObject(o, "lat", l->lat);
        cJSON_AddNumberToObject(o, "lon", l->lon);
        cJSON_AddItemToArray(wl, o);
    }
    char tz[96];
    tz_build_posix(s, tz, sizeof(tz));
    cJSON_AddStringToObject(j, "tz_posix", tz);
    char *out = cJSON_PrintUnformatted(j);
    cJSON_Delete(j);
    return out;
}

static bool set_str(cJSON *j, const char *key, char *dst, size_t n)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsString(v) || !v->valuestring) return false;
    if (strncmp(dst, v->valuestring, n) == 0) return false;
    snprintf(dst, n, "%s", v->valuestring);
    return true;
}

static bool set_u8(cJSON *j, const char *key, uint8_t *dst)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v)) return false;
    uint8_t nv = (uint8_t)v->valuedouble;
    if (nv == *dst) return false;
    *dst = nv;
    return true;
}

static bool set_u16(cJSON *j, const char *key, uint16_t *dst)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsNumber(v)) return false;
    uint16_t nv = (uint16_t)v->valuedouble;
    if (nv == *dst) return false;
    *dst = nv;
    return true;
}

static bool set_bool(cJSON *j, const char *key, bool *dst)
{
    cJSON *v = cJSON_GetObjectItemCaseSensitive(j, key);
    if (!cJSON_IsBool(v)) return false;
    bool nv = cJSON_IsTrue(v);
    if (nv == *dst) return false;
    *dst = nv;
    return true;
}

bool settings_apply_json(settings_t *s, const char *json, bool *wifi_changed)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) return false;
    bool ch = false, wch = false;
    wch |= set_str(j, "wifi_ssid", s->wifi_ssid, sizeof(s->wifi_ssid));
    /* Password only changes when explicitly sent (the UI never echoes it back). */
    wch |= set_str(j, "wifi_pass", s->wifi_pass, sizeof(s->wifi_pass));
    ch |= wch;
    ch |= set_str(j, "hostname", s->hostname, sizeof(s->hostname));
    ch |= set_u8(j, "brightness", &s->brightness);
    ch |= set_u16(j, "screensaver_s", &s->screensaver_s);
    ch |= set_bool(j, "night_dim", &s->night_dim);
    ch |= set_u8(j, "night_start_h", &s->night_start_h);
    ch |= set_u8(j, "night_end_h", &s->night_end_h);
    ch |= set_u8(j, "night_brightness", &s->night_brightness);
    ch |= set_str(j, "tz_id", s->tz_id, sizeof(s->tz_id));
    ch |= set_u8(j, "dst_mode", &s->dst_mode);
    ch |= set_str(j, "tz_custom", s->tz_custom, sizeof(s->tz_custom));
    ch |= set_bool(j, "clock_24h", &s->clock_24h);
    ch |= set_bool(j, "clock_seconds", &s->clock_seconds);
    ch |= set_u8(j, "clock_face", &s->clock_face);
    ch |= set_str(j, "ntp_server", s->ntp_server, sizeof(s->ntp_server));
    ch |= set_str(j, "province", s->province, sizeof(s->province));
    ch |= set_u8(j, "radio_volume", &s->radio_volume);
    ch |= set_bool(j, "radio_resume", &s->radio_resume);
    ch |= set_bool(j, "radio_out_usb", &s->radio_out_usb);
    ch |= set_bool(j, "rtc_vbat", &s->rtc_vbat);
    ch |= set_u8(j, "saver_mode", &s->saver_mode);
    ch |= set_u8(j, "photo_interval_s", &s->photo_interval_s);
    ch |= set_bool(j, "photo_shuffle", &s->photo_shuffle);
    ch |= set_bool(j, "photo_fill", &s->photo_fill);
    ch |= set_bool(j, "photo_recursive", &s->photo_recursive);
    ch |= set_bool(j, "photo_frame_clock", &s->photo_frame_clock);
    ch |= set_str(j, "photo_dir", s->photo_dir, sizeof(s->photo_dir));
    cJSON *al = cJSON_GetObjectItemCaseSensitive(j, "alarms");
    if (cJSON_IsArray(al)) {
        /* the array index is the slot; missing slots are left alone */
        int i = 0;
        cJSON *o;
        cJSON_ArrayForEach(o, al) {
            if (i >= ALARM_MAX) break;
            alarm_t *a = &s->alarms[i++];
            if (!cJSON_IsObject(o)) continue;
            ch |= set_bool(o, "used", &a->used);
            ch |= set_bool(o, "on", &a->on);
            ch |= set_u8(o, "hour", &a->hour);
            ch |= set_u8(o, "minute", &a->minute);
            ch |= set_u8(o, "days", &a->days);
            ch |= set_u8(o, "sound", &a->sound);
            ch |= set_str(o, "label", a->label, sizeof(a->label));
        }
    }
    ch |= set_u8(j, "alarm_snooze_min", &s->alarm_snooze_min);
    ch |= set_u8(j, "alarm_volume", &s->alarm_volume);
    ch |= set_bool(j, "alarm_fade", &s->alarm_fade);
    ch |= set_u8(j, "alarm_ring_min", &s->alarm_ring_min);
    ch |= set_str(j, "wx_key", s->wx_key, sizeof(s->wx_key));
    ch |= set_u8(j, "wx_units", &s->wx_units);
    ch |= set_u8(j, "wx_aq_scale", &s->wx_aq_scale);
    ch |= set_bool(j, "wx_home", &s->wx_home);
    ch |= set_u8(j, "wx_loc_sel", &s->wx_loc_sel);
    cJSON *wl = cJSON_GetObjectItemCaseSensitive(j, "wx_locs");
    if (cJSON_IsArray(wl)) {
        /* the whole list is replaced */
        wx_loc_t nl[WX_LOC_MAX];
        int n = 0;
        cJSON *o;
        memset(nl, 0, sizeof(nl));
        cJSON_ArrayForEach(o, wl) {
            if (n >= WX_LOC_MAX) break;
            cJSON *nm = cJSON_GetObjectItemCaseSensitive(o, "name"), *la = cJSON_GetObjectItemCaseSensitive(o, "lat"),
                  *lo = cJSON_GetObjectItemCaseSensitive(o, "lon");
            if (!cJSON_IsString(nm) || !cJSON_IsNumber(la) || !cJSON_IsNumber(lo)) continue;
            snprintf(nl[n].name, sizeof(nl[n].name), "%s", nm->valuestring);
            set_str(o, "region", nl[n].region, sizeof(nl[n].region));
            set_str(o, "country", nl[n].country, sizeof(nl[n].country));
            nl[n].lat = (float)la->valuedouble;
            nl[n].lon = (float)lo->valuedouble;
            n++;
        }
        if (n != s->wx_loc_count || memcmp(nl, s->wx_locs, sizeof(wx_loc_t) * n)) {
            memset(s->wx_locs, 0, sizeof(s->wx_locs));
            memcpy(s->wx_locs, nl, sizeof(wx_loc_t) * n);
            s->wx_loc_count = (uint8_t)n;
            ch = true;
        }
    }
    cJSON_Delete(j);
    settings_sanitize(s);
    if (wifi_changed) *wifi_changed = wch;
    return ch;
}
