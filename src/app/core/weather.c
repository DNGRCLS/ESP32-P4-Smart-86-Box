/*
 * OpenWeather parsers and weather formulas.  See weather.h.
 */
#include "weather.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

/* ------------------------------------------------------------------ JSON helpers */

static float num(const cJSON *o, const char *k, float def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (float)v->valuedouble : def;
}

static time_t tnum(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? (time_t)v->valuedouble : 0;
}

static void str(const cJSON *o, const char *k, char *dst, size_t n)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsString(v) && v->valuestring) snprintf(dst, n, "%s", v->valuestring);
}

/* "rain": 1.2   or   "rain": {"1h": 1.2}   or   {"3h": ..} */
static float precip(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    if (cJSON_IsNumber(v)) return (float)v->valuedouble;
    if (cJSON_IsObject(v)) {
        float a = num(v, "1h", -1);
        if (a >= 0) return a;
        a = num(v, "3h", -1);
        if (a >= 0) return a;
    }
    return 0;
}

static void cap_first(char *s)
{
    if (s[0]) s[0] = (char)toupper((unsigned char)s[0]);
}

/* weather[0] -> id, icon, description */
static void cond(const cJSON *o, int *id, char *icon, char *desc, size_t dn)
{
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, "weather");
    const cJSON *w = cJSON_IsArray(arr) ? cJSON_GetArrayItem(arr, 0) : NULL;
    if (!w) return;
    if (id) *id = (int)num(w, "id", 0);
    if (icon) str(w, "icon", icon, 4);
    if (desc) { str(w, "description", desc, dn); cap_first(desc); }
}

/* One Call 4.0 answers {"data":[...]} ; tolerate a bare array or a bare object too. */
static const cJSON *oc4_data(const cJSON *root)
{
    if (cJSON_IsArray(root)) return root;
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(root, "data");
    if (cJSON_IsArray(d)) return d;
    return NULL;
}

void wx_api_message(const char *json, char *out, int n)
{
    out[0] = 0;
    cJSON *j = cJSON_Parse(json);
    if (!j) return;
    str(j, "message", out, n);
    cap_first(out);
    cJSON_Delete(j);
}

static bool fail(char *err, int errn, const char *msg)
{
    if (err) snprintf(err, errn, "%s", msg);
    return false;
}

/* ------------------------------------------------------------------ 2.5 current weather */

bool wx_parse_current25(weather_t *w, const char *json, char *err, int errn)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) return fail(err, errn, "Unreadable answer from OpenWeather");
    const cJSON *m = cJSON_GetObjectItemCaseSensitive(j, "main");
    if (!cJSON_IsObject(m)) { cJSON_Delete(j); return fail(err, errn, "No weather in the answer"); }
    w->dt = tnum(j, "dt");
    w->temp = num(m, "temp", WX_NA);
    w->feels = num(m, "feels_like", WX_NA);
    w->humidity = num(m, "humidity", WX_NA);
    w->pressure = num(m, "pressure", WX_NA);
    w->dew_point = (w->temp != WX_NA && w->humidity != WX_NA) ? wx_dew_point(w->temp, w->humidity) : WX_NA;
    const cJSON *wi = cJSON_GetObjectItemCaseSensitive(j, "wind");
    w->wind_ms = num(wi, "speed", WX_NA);
    w->gust_ms = num(wi, "gust", WX_NA);
    w->wind_deg = num(wi, "deg", WX_NA);
    w->visibility_m = num(j, "visibility", WX_NA);
    w->clouds = num(cJSON_GetObjectItemCaseSensitive(j, "clouds"), "all", WX_NA);
    w->rain_1h = precip(j, "rain");
    w->snow_1h = precip(j, "snow");
    w->uvi = WX_NA;
    cond(j, &w->cond_id, w->icon, w->desc, sizeof(w->desc));
    const cJSON *sys = cJSON_GetObjectItemCaseSensitive(j, "sys");
    w->sunrise = tnum(sys, "sunrise");
    w->sunset = tnum(sys, "sunset");
    w->tz_offset = (long)num(j, "timezone", 0);
    str(j, "name", w->place, sizeof(w->place));
    cJSON_Delete(j);
    return w->temp != WX_NA;
}

/* ------------------------------------------------------------------ 2.5 forecast (3-hourly, 5 days) */

static long local_day(time_t t, long off) { return (long)((t + off) / 86400); }
static int local_hour(time_t t, long off) { return (int)(((t + off) % 86400) / 3600); }

bool wx_parse_forecast25(weather_t *w, const char *json, char *err, int errn)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) return fail(err, errn, "Unreadable forecast");
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(j, "list");
    if (!cJSON_IsArray(list)) { cJSON_Delete(j); return fail(err, errn, "No forecast in the answer"); }
    const cJSON *city = cJSON_GetObjectItemCaseSensitive(j, "city");
    if (cJSON_IsObject(city) && cJSON_GetObjectItemCaseSensitive(city, "timezone"))
        w->tz_offset = (long)num(city, "timezone", (float)w->tz_offset);
    long off = w->tz_offset;

    w->n_hours = 0;
    w->hour_step_h = 3;
    w->n_days = 0;
    int best_dist[WX_DAYS_MAX];
    long day_key[WX_DAYS_MAX];
    const cJSON *it;
    cJSON_ArrayForEach(it, list) {
        time_t dt = tnum(it, "dt");
        const cJSON *m = cJSON_GetObjectItemCaseSensitive(it, "main");
        float t = num(m, "temp", WX_NA);
        if (!dt || t == WX_NA) continue;
        float pop = num(it, "pop", 0);
        float rain = precip(it, "rain"), snow = precip(it, "snow");
        char icon[4] = "", desc[40] = "";
        cond(it, NULL, icon, desc, sizeof(desc));

        if (w->n_hours < 8) {   /* next 24 hours */
            wx_hour_t *h = &w->hours[w->n_hours++];
            h->dt = dt;
            h->temp = t;
            h->feels = num(m, "feels_like", t);
            h->pop = pop;
            h->precip_mm = rain + snow;
            snprintf(h->icon, sizeof(h->icon), "%s", icon);
        }

        long key = local_day(dt, off);
        int d = -1;
        for (int i = 0; i < w->n_days; i++) if (day_key[i] == key) d = i;
        if (d < 0) {
            if (w->n_days >= WX_DAYS_MAX) continue;
            d = w->n_days++;
            day_key[d] = key;
            best_dist[d] = 99;
            wx_day_t *nd = &w->days[d];
            memset(nd, 0, sizeof(*nd));
            nd->dt = dt;
            nd->tmin = 999;
            nd->tmax = -999;
            nd->uvi = WX_NA;
            nd->moon_phase = WX_NA;
            nd->wind_ms = 0;
        }
        wx_day_t *dd = &w->days[d];
        float lo = num(m, "temp_min", t), hi = num(m, "temp_max", t);
        if (lo < dd->tmin) dd->tmin = lo;
        if (hi > dd->tmax) dd->tmax = hi;
        if (pop > dd->pop) dd->pop = pop;
        dd->rain_mm += rain;
        dd->snow_mm += snow;
        float ws = num(cJSON_GetObjectItemCaseSensitive(it, "wind"), "speed", 0);
        if (ws > dd->wind_ms) dd->wind_ms = ws;
        dd->humidity = num(m, "humidity", dd->humidity);
        /* the early-afternoon slot best represents the day */
        int dist = abs(local_hour(dt, off) - 13);
        if (dist < best_dist[d]) {
            best_dist[d] = dist;
            snprintf(dd->icon, sizeof(dd->icon), "%s", icon);
            snprintf(dd->desc, sizeof(dd->desc), "%s", desc);
        }
    }
    cJSON_Delete(j);

    /* today: fold in the current reading and the sun times */
    if (w->n_days && w->dt && local_day(w->dt, off) == day_key[0]) {
        if (w->temp != WX_NA) {
            if (w->temp < w->days[0].tmin) w->days[0].tmin = w->temp;
            if (w->temp > w->days[0].tmax) w->days[0].tmax = w->temp;
        }
        w->days[0].sunrise = w->sunrise;
        w->days[0].sunset = w->sunset;
    } else if (w->dt && w->temp != WX_NA && w->n_days < WX_DAYS_MAX) {
        /* late evening: no forecast slot left for today, so start the list with the current reading */
        memmove(&w->days[1], &w->days[0], sizeof(wx_day_t) * (WX_DAYS_MAX - 1));
        if (w->n_days < WX_DAYS_MAX) w->n_days++;
        wx_day_t *t = &w->days[0];
        memset(t, 0, sizeof(*t));
        t->dt = w->dt;
        t->tmin = t->tmax = w->temp;
        t->uvi = WX_NA;
        t->moon_phase = WX_NA;
        t->sunrise = w->sunrise;
        t->sunset = w->sunset;
        snprintf(t->icon, sizeof(t->icon), "%s", w->icon);
        snprintf(t->desc, sizeof(t->desc), "%.39s", w->desc);
    }
    return w->n_days > 0;
}

/* ------------------------------------------------------------------ air pollution */

bool wx_parse_air(weather_t *w, const char *json)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) return false;
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(j, "list");
    const cJSON *e = cJSON_IsArray(list) ? cJSON_GetArrayItem(list, 0) : NULL;
    bool ok = false;
    if (e) {
        w->owm_aqi = (int)num(cJSON_GetObjectItemCaseSensitive(e, "main"), "aqi", 0);
        const cJSON *c = cJSON_GetObjectItemCaseSensitive(e, "components");
        w->pm2_5 = num(c, "pm2_5", WX_NA);
        w->pm10 = num(c, "pm10", WX_NA);
        w->o3 = num(c, "o3", WX_NA);
        w->no2 = num(c, "no2", WX_NA);
        w->so2 = num(c, "so2", WX_NA);
        w->co = num(c, "co", WX_NA);
        ok = w->owm_aqi > 0;
    }
    w->aq_valid = ok;
    cJSON_Delete(j);
    return ok;
}

/* ------------------------------------------------------------------ One Call (3.0 and 4.0 share field names) */

static void oc_current(weather_t *w, const cJSON *c)
{
    w->dt = tnum(c, "dt");
    w->sunrise = tnum(c, "sunrise") ? tnum(c, "sunrise") : w->sunrise;
    w->sunset = tnum(c, "sunset") ? tnum(c, "sunset") : w->sunset;
    w->temp = num(c, "temp", w->temp);
    w->feels = num(c, "feels_like", w->feels);
    w->pressure = num(c, "pressure", w->pressure);
    w->humidity = num(c, "humidity", w->humidity);
    w->dew_point = num(c, "dew_point", w->dew_point);
    w->uvi = num(c, "uvi", WX_NA);
    w->clouds = num(c, "clouds", w->clouds);
    w->visibility_m = num(c, "visibility", w->visibility_m);
    w->wind_ms = num(c, "wind_speed", w->wind_ms);
    w->wind_deg = num(c, "wind_deg", w->wind_deg);
    w->gust_ms = num(c, "wind_gust", WX_NA);
    w->rain_1h = precip(c, "rain");
    w->snow_1h = precip(c, "snow");
    cond(c, &w->cond_id, w->icon, w->desc, sizeof(w->desc));
}

static void oc_hour(wx_hour_t *h, const cJSON *o)
{
    h->dt = tnum(o, "dt");
    h->temp = num(o, "temp", WX_NA);
    h->feels = num(o, "feels_like", h->temp);
    h->pop = num(o, "pop", 0);
    h->precip_mm = precip(o, "rain") + precip(o, "snow");
    cond(o, NULL, h->icon, NULL, 0);
}

static void oc_day(wx_day_t *d, const cJSON *o)
{
    memset(d, 0, sizeof(*d));
    d->dt = tnum(o, "dt");
    const cJSON *t = cJSON_GetObjectItemCaseSensitive(o, "temp");
    d->tmin = num(t, "min", WX_NA);
    d->tmax = num(t, "max", WX_NA);
    d->pop = num(o, "pop", 0);
    d->rain_mm = precip(o, "rain");
    d->snow_mm = precip(o, "snow");
    d->uvi = num(o, "uvi", WX_NA);
    d->wind_ms = num(o, "wind_speed", 0);
    d->humidity = num(o, "humidity", 0);
    d->sunrise = tnum(o, "sunrise");
    d->sunset = tnum(o, "sunset");
    d->moon_phase = num(o, "moon_phase", WX_NA);
    cond(o, NULL, d->icon, d->desc, sizeof(d->desc));
}

static void alert_text(weather_t *w, const cJSON *a)
{
    str(a, "event", w->alert_event, sizeof(w->alert_event));
    str(a, "sender_name", w->alert_sender, sizeof(w->alert_sender));
    w->alert_start = tnum(a, "start");
    w->alert_end = tnum(a, "end");
    const cJSON *d = cJSON_GetObjectItemCaseSensitive(a, "description");
    if (cJSON_IsString(d)) {
        snprintf(w->alert_desc, sizeof(w->alert_desc), "%s", d->valuestring);
    } else if (cJSON_IsArray(d)) {
        /* 4.0: one entry per language; prefer English */
        const cJSON *pick = cJSON_GetArrayItem(d, 0), *e;
        cJSON_ArrayForEach(e, d) {
            const cJSON *l = cJSON_GetObjectItemCaseSensitive(e, "language");
            if (cJSON_IsString(l) && !strncmp(l->valuestring, "en", 2)) { pick = e; break; }
        }
        if (cJSON_IsString(pick)) snprintf(w->alert_desc, sizeof(w->alert_desc), "%s", pick->valuestring);
        else if (cJSON_IsObject(pick)) {
            str(pick, "description", w->alert_desc, sizeof(w->alert_desc));
            if (!w->alert_event[0]) str(pick, "event", w->alert_event, sizeof(w->alert_event));
            if (!w->alert_desc[0]) str(pick, "text", w->alert_desc, sizeof(w->alert_desc));
        }
    }
    for (char *p = w->alert_desc; *p; p++) if (*p == '\r') *p = ' ';
}

bool wx_parse_onecall3(weather_t *w, const char *json, char *err, int errn)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) return fail(err, errn, "Unreadable One Call answer");
    const cJSON *c = cJSON_GetObjectItemCaseSensitive(j, "current");
    if (!cJSON_IsObject(c)) { cJSON_Delete(j); return fail(err, errn, "No current weather in One Call"); }
    if (cJSON_GetObjectItemCaseSensitive(j, "timezone_offset")) w->tz_offset = (long)num(j, "timezone_offset", 0);
    oc_current(w, c);
    const cJSON *hl = cJSON_GetObjectItemCaseSensitive(j, "hourly"), *it;
    w->n_hours = 0;
    w->hour_step_h = 1;
    cJSON_ArrayForEach(it, hl) {
        if (w->n_hours >= WX_HOURS_MAX) break;
        oc_hour(&w->hours[w->n_hours++], it);
    }
    const cJSON *dl = cJSON_GetObjectItemCaseSensitive(j, "daily");
    w->n_days = 0;
    cJSON_ArrayForEach(it, dl) {
        if (w->n_days >= WX_DAYS_MAX) break;
        oc_day(&w->days[w->n_days++], it);
    }
    w->alert_event[0] = w->alert_sender[0] = w->alert_desc[0] = 0;
    w->alert_id[0] = 0;
    const cJSON *al = cJSON_GetObjectItemCaseSensitive(j, "alerts");
    if (cJSON_IsArray(al) && cJSON_GetArraySize(al) > 0) alert_text(w, cJSON_GetArrayItem(al, 0));
    cJSON_Delete(j);
    return true;
}

bool wx_parse_oc4_current(weather_t *w, const char *json, char *err, int errn)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) return fail(err, errn, "Unreadable One Call answer");
    const cJSON *d = oc4_data(j);
    const cJSON *c = d ? cJSON_GetArrayItem(d, 0) : (cJSON_IsObject(j) && cJSON_GetObjectItemCaseSensitive(j, "temp") ? j : NULL);
    if (!c) { cJSON_Delete(j); return fail(err, errn, "No current weather in One Call"); }
    oc_current(w, c);
    /* alerts: a list of ids; the text comes from alert/{id} */
    char prev[48];
    snprintf(prev, sizeof(prev), "%s", w->alert_id);
    w->alert_id[0] = 0;
    const cJSON *al = cJSON_GetObjectItemCaseSensitive(c, "alerts");
    const cJSON *a0 = cJSON_IsArray(al) ? cJSON_GetArrayItem(al, 0) : NULL;
    if (cJSON_IsString(a0)) snprintf(w->alert_id, sizeof(w->alert_id), "%s", a0->valuestring);
    else if (cJSON_IsNumber(a0)) snprintf(w->alert_id, sizeof(w->alert_id), "%.0f", a0->valuedouble);
    else if (cJSON_IsObject(a0)) {
        str(a0, "id", w->alert_id, sizeof(w->alert_id));
        if (cJSON_GetObjectItemCaseSensitive(a0, "event")) alert_text(w, a0);
    }
    if (!w->alert_id[0] || strcmp(prev, w->alert_id)) {
        if (!(cJSON_IsObject(a0) && w->alert_event[0])) w->alert_event[0] = w->alert_sender[0] = w->alert_desc[0] = 0;
    }
    cJSON_Delete(j);
    return true;
}

bool wx_parse_oc4_hourly(weather_t *w, const char *json)
{
    cJSON *j = cJSON_Parse(json);
    const cJSON *d = j ? oc4_data(j) : NULL, *it;
    if (!d) { cJSON_Delete(j); return false; }
    w->n_hours = 0;
    w->hour_step_h = 1;
    cJSON_ArrayForEach(it, d) {
        if (w->n_hours >= WX_HOURS_MAX) break;
        oc_hour(&w->hours[w->n_hours], it);
        if (w->hours[w->n_hours].temp != WX_NA) w->n_hours++;
    }
    cJSON_Delete(j);
    return w->n_hours > 0;
}

bool wx_parse_oc4_daily(weather_t *w, const char *json)
{
    cJSON *j = cJSON_Parse(json);
    const cJSON *d = j ? oc4_data(j) : NULL, *it;
    if (!d) { cJSON_Delete(j); return false; }
    w->n_days = 0;
    cJSON_ArrayForEach(it, d) {
        if (w->n_days >= WX_DAYS_MAX) break;
        oc_day(&w->days[w->n_days], it);
        if (w->days[w->n_days].tmax != WX_NA) w->n_days++;
    }
    cJSON_Delete(j);
    return w->n_days > 0;
}

bool wx_parse_oc4_alert(weather_t *w, const char *json)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) return false;
    const cJSON *d = oc4_data(j);
    const cJSON *a = d ? cJSON_GetArrayItem(d, 0) : j;
    w->alert_event[0] = w->alert_sender[0] = w->alert_desc[0] = 0;
    if (cJSON_IsObject(a)) alert_text(w, a);
    cJSON_Delete(j);
    return w->alert_event[0] != 0;
}

/* ------------------------------------------------------------------ geocoding */

static void geo_one(const cJSON *o, wx_loc_t *l)
{
    memset(l, 0, sizeof(*l));
    str(o, "name", l->name, sizeof(l->name));
    str(o, "state", l->region, sizeof(l->region));
    str(o, "country", l->country, sizeof(l->country));
    l->lat = num(o, "lat", 0);
    l->lon = num(o, "lon", 0);
}

int wx_parse_geocode(const char *json, wx_loc_t *out, int max)
{
    cJSON *j = cJSON_Parse(json);
    if (!j) return -1;
    int n = 0;
    if (cJSON_IsArray(j)) {
        const cJSON *it;
        cJSON_ArrayForEach(it, j) {
            if (n >= max) break;
            geo_one(it, &out[n]);
            if (!out[n].name[0]) continue;
            /* the same town often comes back twice with slightly different coordinates */
            bool dup = false;
            for (int i = 0; i < n; i++)
                if (!strcmp(out[i].name, out[n].name) && !strcmp(out[i].region, out[n].region) &&
                    !strcmp(out[i].country, out[n].country)) dup = true;
            if (!dup) n++;
        }
    } else if (cJSON_IsObject(j) && cJSON_GetObjectItemCaseSensitive(j, "lat") && max > 0) {
        geo_one(j, &out[0]);
        n = out[0].name[0] ? 1 : 0;
    }
    cJSON_Delete(j);
    return n;
}

bool wx_zip_query(const char *q, char *zip, int n)
{
    char s[24];
    int k = 0;
    for (const char *p = q; *p && k < (int)sizeof(s) - 1; p++)
        if (!isspace((unsigned char)*p)) s[k++] = (char)toupper((unsigned char)*p);
    s[k] = 0;
    /* explicit "code,CC" */
    const char *comma = strchr(s, ',');
    if (comma && strlen(comma + 1) == 2) {
        bool digit = false;
        for (const char *p = s; p < comma; p++) if (isdigit((unsigned char)*p)) digit = true;
        if (digit) { snprintf(zip, n, "%s", s); return true; }
    }
    /* Canada: A1A or A1A1A1 -> the API knows the first three characters (FSA) */
    if ((k == 3 || k == 6) && isalpha((unsigned char)s[0]) && isdigit((unsigned char)s[1]) && isalpha((unsigned char)s[2]) &&
        (k == 3 || (isdigit((unsigned char)s[3]) && isalpha((unsigned char)s[4]) && isdigit((unsigned char)s[5])))) {
        snprintf(zip, n, "%.3s,CA", s);
        return true;
    }
    /* USA: 12345 or 12345-6789 */
    if (k >= 5 && isdigit((unsigned char)s[0]) && isdigit((unsigned char)s[1]) && isdigit((unsigned char)s[2]) &&
        isdigit((unsigned char)s[3]) && isdigit((unsigned char)s[4]) && (k == 5 || s[5] == '-')) {
        snprintf(zip, n, "%.5s,US", s);
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ formulas */

float wx_dew_point(float t, float rh)
{
    if (rh < 1) rh = 1;
    const float a = 17.62f, b = 243.12f;
    float g = logf(rh / 100.0f) + a * t / (b + t);
    return b * g / (a - g);
}

float wx_humidex(float t, float dew)
{
    float e = 6.11f * expf(5417.7530f * ((1.0f / 273.16f) - (1.0f / (273.15f + dew))));
    return t + 0.5555f * (e - 10.0f);
}

bool wx_humidex_applies(float t, float h) { return t >= 20.0f && h >= 25.0f && h > t + 0.5f; }

float wx_wind_chill(float t, float v)
{
    float p = powf(v, 0.16f);
    return 13.12f + 0.6215f * t - 11.37f * p + 0.3965f * t * p;
}

bool wx_wind_chill_applies(float t, float v) { return t <= 0.0f && v >= 5.0f; }

float wx_heat_index_c(float t_c, float rh)
{
    float T = t_c * 9.0f / 5.0f + 32.0f, R = rh;
    float hi = 0.5f * (T + 61.0f + (T - 68.0f) * 1.2f + R * 0.094f);
    if ((hi + T) / 2.0f >= 80.0f) {
        hi = -42.379f + 2.04901523f * T + 10.14333127f * R - 0.22475541f * T * R - 0.00683783f * T * T -
             0.05481717f * R * R + 0.00122874f * T * T * R + 0.00085282f * T * R * R - 0.00000199f * T * T * R * R;
        if (R < 13 && T >= 80 && T <= 112) hi -= ((13 - R) / 4) * sqrtf((17 - fabsf(T - 95.0f)) / 17);
        else if (R > 85 && T >= 80 && T <= 87) hi += ((R - 85) / 10) * ((87 - T) / 5);
    }
    return (hi - 32.0f) * 5.0f / 9.0f;
}

bool wx_heat_index_applies(float t_c, float hi_c) { return t_c >= 26.7f && hi_c > t_c + 0.5f; }   /* 80 °F */

int wx_aqhi(float o3, float no2, float pm25)
{
    if (o3 < 0 || no2 < 0 || pm25 < 0) return 0;
    float o3ppb = o3 / 1.996f, no2ppb = no2 / 1.88f;   /* µg/m³ -> ppb at 25 °C */
    float v = (1000.0f / 10.4f) * ((expf(0.000537f * o3ppb) - 1) + (expf(0.000871f * no2ppb) - 1) + (expf(0.000487f * pm25) - 1));
    int a = (int)lroundf(v);
    if (a < 1) a = 1;
    if (a > 11) a = 11;
    return a;
}

const char *wx_aqhi_risk(int a)
{
    if (a <= 3) return "Low risk";
    if (a <= 6) return "Moderate risk";
    if (a <= 10) return "High risk";
    return "Very high risk";
}

static int aqi_scale(float c, const float *bp, int nbp)
{
    static const int lo[] = {0, 51, 101, 151, 201, 301}, hi[] = {50, 100, 150, 200, 300, 500};
    if (c < 0) return 0;
    for (int i = 0; i < nbp; i++) {
        float cl = i ? bp[i - 1] : 0, ch = bp[i];
        if (c <= ch) return (int)lroundf((hi[i] - lo[i]) * (c - cl) / (ch - cl) + lo[i]);
    }
    return 500;
}

int wx_us_aqi(float pm25, float pm10, float o3)
{
    /* EPA breakpoints (PM2.5 as revised in 2024); concentrations treated as the averages */
    static const float bp25[] = {9.0f, 35.4f, 55.4f, 125.4f, 225.4f, 325.4f};
    static const float bp10[] = {54, 154, 254, 354, 424, 604};
    static const float bpo3[] = {54, 70, 85, 105, 200};     /* ppb, 8 hour */
    int a = aqi_scale(pm25, bp25, 6);
    int b = aqi_scale(pm10, bp10, 6);
    int c = o3 >= 0 ? aqi_scale(o3 / 1.996f, bpo3, 5) : 0;
    int m = a > b ? a : b;
    return m > c ? m : c;
}

const char *wx_us_aqi_label(int a)
{
    if (a <= 50) return "Good";
    if (a <= 100) return "Moderate";
    if (a <= 150) return "Unhealthy for sensitive groups";
    if (a <= 200) return "Unhealthy";
    if (a <= 300) return "Very unhealthy";
    return "Hazardous";
}

const char *wx_owm_aqi_label(int a)
{
    static const char *n[] = {"-", "Good", "Fair", "Moderate", "Poor", "Very poor"};
    return n[a < 0 || a > 5 ? 0 : a];
}

const char *wx_uv_label(float u)
{
    if (u < 0) return "-";
    if (u < 3) return "Low";
    if (u < 6) return "Moderate";
    if (u < 8) return "High";
    if (u < 11) return "Very high";
    return "Extreme";
}

const char *wx_compass(float deg)
{
    static const char *d[] = {"N", "NNE", "NE", "ENE", "E", "ESE", "SE", "SSE", "S", "SSW", "SW", "WSW", "W", "WNW", "NW", "NNW"};
    if (deg < 0) return "";
    int i = (int)((fmodf(deg, 360.0f) + 11.25f) / 22.5f) % 16;
    return d[i];
}

const char *wx_moon_name(float p)
{
    if (p < 0) return "";
    if (p < 0.03f || p > 0.97f) return "New moon";
    if (p < 0.22f) return "Waxing crescent";
    if (p < 0.28f) return "First quarter";
    if (p < 0.47f) return "Waxing gibbous";
    if (p < 0.53f) return "Full moon";
    if (p < 0.72f) return "Waning gibbous";
    if (p < 0.78f) return "Last quarter";
    return "Waning crescent";
}
