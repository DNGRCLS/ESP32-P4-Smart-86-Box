/*
 * Weather fetcher (OpenWeather).
 *
 * Free plan (any key):      2.5 current weather, 2.5 forecast (3-hourly, 5 days), air pollution, geocoding.
 * One Call subscription:    4.0 current + timeline/1h + timeline/1day (+ alert details), else 3.0 onecall.
 *                           Adds the UV index, hourly steps, 8-10 days, moon phase and alerts.
 * The panel finds out by itself which of these the key may use: it tries One Call 4.0, then 3.0,
 * and if both are refused it uses the free APIs and asks again every 6 hours.
 *
 * Calls per refresh (every 15 minutes, for the place selected):
 *   free plan only : 3 free calls                          -> ~290 a day
 *   One Call 4.0   : 3 One Call + 2 free (+1 per new alert) -> ~290 One Call calls a day
 *   One Call 3.0   : 1 One Call + 2 free (+ the 3-hourly forecast if needed)
 * all well inside the 1,000 free One Call calls a day.
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "../../drivers/esp_lvgl_port/esp_lvgl_port.h"
#include "../core/platform.h"
#include "../core/settings.h"
#include "../core/weather.h"
#include "weather_net.h"

static const char *TAG = "weather";

#define HOST         "https://api.openweathermap.org"
#define REFRESH_S    (15 * 60)
#define RETRY_S      120
#define ONECALL_RECHECK_S (6 * 3600)
#define BODY_MAX     (320 * 1024)

typedef enum { OC_UNKNOWN = 0, OC_V4, OC_V3, OC_NONE } oc_mode_t;
typedef enum { J_REFRESH, J_SEARCH, J_RESET } job_type_t;

typedef struct {
    job_type_t type;
    bool force;
    char q[64];
    wx_search_cb_t cb;           /* panel search: result delivered in LVGL context */
    void *user;
    SemaphoreHandle_t done;      /* web search: caller waits on this */
    wx_loc_t *out;
    int max;
    int *n;
    char *err;
    int errn;
} job_t;

static QueueHandle_t s_q;
static SemaphoreHandle_t s_mtx;
static weather_t *s_cache;       /* WX_LOC_MAX entries in PSRAM */
static volatile uint32_t s_gen;
static volatile bool s_busy;
static oc_mode_t s_oc = OC_UNKNOWN;
static time_t s_oc_retry;
static char s_oc_note[64] = "not checked yet";

#define LOCK() xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

/* ------------------------------------------------------------------ HTTP */

typedef struct {
    char *buf;
    int len, cap;
} grow_t;

static esp_err_t http_evt(esp_http_client_event_t *e)
{
    grow_t *g = (grow_t *)e->user_data;
    if (e->event_id == HTTP_EVENT_ON_DATA && g) {
        if (g->len + e->data_len + 1 > g->cap) {
            int ncap = (g->len + e->data_len + 1) * 2;
            if (ncap < 8192) ncap = 8192;
            if (ncap > BODY_MAX) return ESP_FAIL;
            char *nb = heap_caps_realloc(g->buf, ncap, MALLOC_CAP_SPIRAM);
            if (!nb) return ESP_FAIL;
            g->buf = nb;
            g->cap = ncap;
        }
        memcpy(g->buf + g->len, e->data, e->data_len);
        g->len += e->data_len;
        g->buf[g->len] = 0;
    }
    return ESP_OK;
}

/* Returns the HTTP status (or -1 when the server could not be reached); *body must be freed. */
static int http_get(const char *url, char **body)
{
    grow_t g = {0};
    esp_http_client_config_t cfg = {
        .url = url,
        .event_handler = http_evt,
        .user_data = &g,
        .timeout_ms = 12000,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .user_agent = "P4Box/1.2 (ESP32-P4)",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return -1;
    esp_err_t e = esp_http_client_perform(c);
    int st = e == ESP_OK ? esp_http_client_get_status_code(c) : -1;
    esp_http_client_cleanup(c);
    if (st < 0) ESP_LOGW(TAG, "GET failed: %s", esp_err_to_name(e));
    *body = g.buf;
    return st;
}

static void url_encode(const char *in, char *out, size_t n)
{
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (; *in && o + 4 < n; in++) {
        unsigned char ch = (unsigned char)*in;
        if (isalnum(ch) || ch == '-' || ch == '_' || ch == '.' || ch == ',') out[o++] = ch;
        else { out[o++] = '%'; out[o++] = hex[ch >> 4]; out[o++] = hex[ch & 15]; }
    }
    out[o] = 0;
}

/* A friendly message for a failed call. */
static void http_error(int st, const char *body, char *err, size_t n)
{
    char m[96] = "";
    if (body) wx_api_message(body, m, sizeof(m));
    if (st == -1) snprintf(err, n, "Can't reach OpenWeather - check the network");
    else if (st == 401) snprintf(err, n, "API key not accepted. New keys can take up to 2 hours to start working");
    else if (st == 429) snprintf(err, n, "Too many requests for this key - try again later");
    else if (st == 404) snprintf(err, n, "OpenWeather has no data for this place");
    else snprintf(err, n, "OpenWeather error %d%s%s", st, m[0] ? ": " : "", m);
}

/* ------------------------------------------------------------------ settings snapshot */

typedef struct {
    char key[48];
    wx_loc_t loc;
    bool has_loc;
} snap_t;

static void snapshot(snap_t *s)
{
    memset(s, 0, sizeof(*s));
    lvgl_port_lock(0);
    snprintf(s->key, sizeof(s->key), "%s", g_settings.wx_key);
    if (g_settings.wx_loc_count > 0) {
        s->loc = g_settings.wx_locs[g_settings.wx_loc_sel < g_settings.wx_loc_count ? g_settings.wx_loc_sel : 0];
        s->has_loc = true;
    }
    lvgl_port_unlock();
}

/* ------------------------------------------------------------------ cache */

static bool same_place(const weather_t *w, const wx_loc_t *l)
{
    return w->valid && w->lat > l->lat - 0.0005f && w->lat < l->lat + 0.0005f && w->lon > l->lon - 0.0005f &&
           w->lon < l->lon + 0.0005f;
}

static int cache_slot(const wx_loc_t *l, bool create)
{
    int oldest = 0;
    for (int i = 0; i < WX_LOC_MAX; i++) {
        if (same_place(&s_cache[i], l)) return i;
        if (!s_cache[i].valid || s_cache[i].fetched < s_cache[oldest].fetched) oldest = i;
    }
    return create ? oldest : -1;
}

static void store_error(const wx_loc_t *l, const char *err)
{
    LOCK();
    int i = cache_slot(l, false);
    if (i < 0) {
        i = cache_slot(l, true);
        memset(&s_cache[i], 0, sizeof(weather_t));
        s_cache[i].valid = true;     /* an entry that only holds the error */
        s_cache[i].lat = l->lat;
        s_cache[i].lon = l->lon;
        s_cache[i].temp = WX_NA;
    }
    snprintf(s_cache[i].error, sizeof(s_cache[i].error), "%s", err);
    s_cache[i].fetched = time(NULL) - REFRESH_S + RETRY_S;   /* retry in a couple of minutes */
    s_gen++;
    UNLOCK();
}

/* ------------------------------------------------------------------ refresh */

static bool onecall4(weather_t *w, const char *ll, const char *key, int *status)
{
    char url[320], *b = NULL;
    snprintf(url, sizeof(url), HOST "/data/4.0/onecall/current?%s&units=metric&appid=%s", ll, key);
    int st = http_get(url, &b);
    *status = st;
    char err[64];
    bool ok = st == 200 && b && wx_parse_oc4_current(w, b, err, sizeof(err));
    free(b);
    if (!ok) return false;
    bool hours = false, days = false;
    snprintf(url, sizeof(url), HOST "/data/4.0/onecall/timeline/1h?%s&units=metric&appid=%s", ll, key);
    b = NULL;
    if (http_get(url, &b) == 200 && b) hours = wx_parse_oc4_hourly(w, b);
    free(b);
    snprintf(url, sizeof(url), HOST "/data/4.0/onecall/timeline/1day?%s&units=metric&appid=%s", ll, key);
    b = NULL;
    if (http_get(url, &b) == 200 && b) days = wx_parse_oc4_daily(w, b);
    free(b);
    if (!hours) w->n_hours = 0;
    if (!days) w->n_days = 0;
    return true;
}

static bool onecall3(weather_t *w, const char *ll, const char *key, int *status)
{
    char url[320], *b = NULL, err[64];
    snprintf(url, sizeof(url), HOST "/data/3.0/onecall?%s&exclude=minutely&units=metric&appid=%s", ll, key);
    int st = http_get(url, &b);
    *status = st;
    bool ok = st == 200 && b && wx_parse_onecall3(w, b, err, sizeof(err));
    free(b);
    return ok;
}

static void do_refresh(bool force)
{
    snap_t sn;
    snapshot(&sn);
    if (!sn.key[0] || !sn.has_loc) return;
    net_status_t ns;
    plat_net_get_status(&ns);
    if (ns.active == NET_ACTIVE_NONE) { store_error(&sn.loc, "Offline - waiting for the network"); return; }
    if (!ns.time_synced) { store_error(&sn.loc, "Waiting for network time"); return; }

    time_t now = time(NULL);
    LOCK();
    int ci = cache_slot(&sn.loc, false);
    bool fresh = ci >= 0 && s_cache[ci].fetched && now - s_cache[ci].fetched < REFRESH_S;
    char prev_alert_id[48] = "", prev_event[64] = "", prev_sender[64] = "", prev_desc[400] = "";
    if (ci >= 0) {
        snprintf(prev_alert_id, sizeof(prev_alert_id), "%s", s_cache[ci].alert_id);
        snprintf(prev_event, sizeof(prev_event), "%s", s_cache[ci].alert_event);
        snprintf(prev_sender, sizeof(prev_sender), "%s", s_cache[ci].alert_sender);
        snprintf(prev_desc, sizeof(prev_desc), "%s", s_cache[ci].alert_desc);
    }
    UNLOCK();
    if (fresh && !force) return;

    s_busy = true;
    s_gen++;
    weather_t *w = heap_caps_calloc(1, sizeof(weather_t), MALLOC_CAP_SPIRAM);
    char *b = NULL, url[320], ll[64], err[96] = "";
    snprintf(ll, sizeof(ll), "lat=%.4f&lon=%.4f", sn.loc.lat, sn.loc.lon);
    w->lat = sn.loc.lat;
    w->lon = sn.loc.lon;

    /* 1. current weather (free): also gives the local time offset and a place name */
    snprintf(url, sizeof(url), HOST "/data/2.5/weather?%s&units=metric&appid=%s", ll, sn.key);
    int st = http_get(url, &b);
    if (st != 200 || !b || !wx_parse_current25(w, b, err, sizeof(err))) {
        if (st != 200) http_error(st, b, err, sizeof(err));
        free(b);
        ESP_LOGW(TAG, "refresh failed: %s", err);
        store_error(&sn.loc, err);
        heap_caps_free(w);
        s_busy = false;
        return;
    }
    free(b);
    b = NULL;

    /* 2. One Call, when this key is allowed to use it */
    bool oc = false, refused4 = false;
    if (s_oc == OC_NONE && now >= s_oc_retry) s_oc = OC_UNKNOWN;
    if (s_oc == OC_UNKNOWN || s_oc == OC_V4) {
        int s4;
        if (onecall4(w, ll, sn.key, &s4)) {
            oc = true;
            s_oc = OC_V4;
            w->source = WX_SRC_ONECALL4;
        } else if (s4 == 401 || s4 == 403 || s4 == 404) {
            refused4 = true;
            if (s_oc == OC_V4) s_oc = OC_UNKNOWN;
        }   /* anything else is network trouble: use the free data this time, try again next time */
    }
    if (!oc && (s_oc == OC_V3 || (s_oc == OC_UNKNOWN && refused4))) {
        int s3;
        if (onecall3(w, ll, sn.key, &s3)) {
            oc = true;
            s_oc = OC_V3;
            w->source = WX_SRC_ONECALL3;
        } else if (s3 == 401 || s3 == 403) {
            s_oc = OC_NONE;
            s_oc_retry = now + ONECALL_RECHECK_S;
            ESP_LOGI(TAG, "this key has no One Call subscription: using the free APIs");
        }
    }
    snprintf(s_oc_note, sizeof(s_oc_note), "%s",
             s_oc == OC_V4 ? "One Call 4.0" : s_oc == OC_V3 ? "One Call 3.0" : s_oc == OC_NONE ? "not on this key" : "not checked yet");

    /* 3. free 5-day forecast when One Call gave no hours or days */
    if (!oc || w->n_days == 0 || w->n_hours == 0) {
        weather_t *f = heap_caps_calloc(1, sizeof(weather_t), MALLOC_CAP_SPIRAM);
        *f = *w;
        snprintf(url, sizeof(url), HOST "/data/2.5/forecast?%s&units=metric&appid=%s", ll, sn.key);
        if (http_get(url, &b) == 200 && b && wx_parse_forecast25(f, b, err, sizeof(err))) {
            if (w->n_hours == 0) { memcpy(w->hours, f->hours, sizeof(w->hours)); w->n_hours = f->n_hours; w->hour_step_h = f->hour_step_h; }
            if (w->n_days == 0) { memcpy(w->days, f->days, sizeof(w->days)); w->n_days = f->n_days; }
        }
        free(b);
        b = NULL;
        heap_caps_free(f);
        if (!oc) w->source = WX_SRC_FREE;
    }

    /* 4. air quality (free) */
    snprintf(url, sizeof(url), HOST "/data/2.5/air_pollution?%s&appid=%s", ll, sn.key);
    if (http_get(url, &b) == 200 && b) wx_parse_air(w, b);
    free(b);
    b = NULL;

    /* 5. One Call 4.0 only lists alert ids: fetch the text of a new one */
    if (w->alert_id[0] && !w->alert_event[0]) {
        if (!strcmp(prev_alert_id, w->alert_id) && prev_event[0]) {
            snprintf(w->alert_event, sizeof(w->alert_event), "%s", prev_event);
            snprintf(w->alert_sender, sizeof(w->alert_sender), "%s", prev_sender);
            snprintf(w->alert_desc, sizeof(w->alert_desc), "%s", prev_desc);
        } else {
            char id[100];
            url_encode(w->alert_id, id, sizeof(id));
            snprintf(url, sizeof(url), HOST "/data/4.0/onecall/alert/%s?appid=%s", id, sn.key);
            if (http_get(url, &b) == 200 && b) wx_parse_oc4_alert(w, b);
            free(b);
            b = NULL;
        }
    }

    w->valid = true;
    w->fetched = time(NULL);
    w->error[0] = 0;
    LOCK();
    int slot = cache_slot(&sn.loc, true);
    s_cache[slot] = *w;
    s_gen++;
    UNLOCK();
    ESP_LOGI(TAG, "%s: %.1f C, %s (%s, %d h / %d d, AQ %d, UV %.1f)", w->place, w->temp, w->desc,
             w->source == WX_SRC_ONECALL4 ? "One Call 4.0" : w->source == WX_SRC_ONECALL3 ? "One Call 3.0" : "free plan",
             w->n_hours, w->n_days, w->owm_aqi, w->uvi);
    heap_caps_free(w);
    s_busy = false;
}

/* ------------------------------------------------------------------ search */

static int do_search(const char *q, wx_loc_t *out, int max, char *err, int errn)
{
    snap_t sn;
    snapshot(&sn);
    if (!sn.key[0]) { snprintf(err, errn, "Add your OpenWeather API key first"); return -1; }
    char url[320], zip[24], enc[200], *b = NULL;
    if (wx_zip_query(q, zip, sizeof(zip))) {
        url_encode(zip, enc, sizeof(enc));
        snprintf(url, sizeof(url), HOST "/geo/1.0/zip?zip=%s&appid=%s", enc, sn.key);
    } else {
        url_encode(q, enc, sizeof(enc));
        snprintf(url, sizeof(url), HOST "/geo/1.0/direct?q=%s&limit=5&appid=%s", enc, sn.key);
    }
    int st = http_get(url, &b);
    int n = -1;
    if (st == 200 && b) {
        n = wx_parse_geocode(b, out, max);
        if (n == 0) snprintf(err, errn, "No place found - try \"Town, Province\" or a postal code");
        if (n < 0) snprintf(err, errn, "Unreadable answer from OpenWeather");
    } else if (st == 404) {
        n = 0;
        snprintf(err, errn, "No place found for that code");
    } else {
        http_error(st, b, err, errn);
    }
    free(b);
    return n;
}

/* ------------------------------------------------------------------ task */

static void weather_task(void *arg)
{
    (void)arg;
    wx_loc_t *res = heap_caps_calloc(8, sizeof(wx_loc_t), MALLOC_CAP_SPIRAM);
    for (;;) {
        job_t j;
        if (xQueueReceive(s_q, &j, pdMS_TO_TICKS(20000)) != pdTRUE) {
            do_refresh(false);   /* keeps the location on screen up to date */
            continue;
        }
        if (j.type == J_RESET) {
            LOCK();
            memset(s_cache, 0, sizeof(weather_t) * WX_LOC_MAX);
            s_gen++;
            UNLOCK();
            s_oc = OC_UNKNOWN;
            snprintf(s_oc_note, sizeof(s_oc_note), "not checked yet");
            do_refresh(true);
        } else if (j.type == J_REFRESH) {
            do_refresh(j.force);
        } else if (j.type == J_SEARCH) {
            char err[96] = "";
            int n = do_search(j.q, res, 8, err, sizeof(err));
            if (j.done) {
                if (j.out && n > 0) memcpy(j.out, res, sizeof(wx_loc_t) * (n < j.max ? n : j.max));
                if (j.n) *j.n = n < j.max ? n : j.max;
                if (j.err) snprintf(j.err, j.errn, "%s", err);
                xSemaphoreGive(j.done);
            } else if (j.cb && lvgl_port_lock(0)) {
                j.cb(res, n < 0 ? 0 : n, n > 0 ? NULL : err, j.user);
                lvgl_port_unlock();
            }
        }
    }
}

/* ------------------------------------------------------------------ public */

void weather_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_q = xQueueCreate(6, sizeof(job_t));
    s_cache = heap_caps_calloc(WX_LOC_MAX, sizeof(weather_t), MALLOC_CAP_SPIRAM);
    /* The task never writes to the flash, so its stack can live in PSRAM. */
    xTaskCreatePinnedToCoreWithCaps(weather_task, "weather", 10 * 1024, NULL, 3, NULL, 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

void plat_weather_get(const wx_loc_t *loc, weather_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_mtx || !loc) return;
    LOCK();
    int i = cache_slot(loc, false);
    if (i >= 0) *out = s_cache[i];
    UNLOCK();
}

uint32_t plat_weather_gen(void) { return s_gen; }
bool plat_weather_busy(void) { return s_busy; }
const char *plat_weather_onecall(void) { return s_oc_note; }

void plat_weather_refresh(bool force)
{
    if (!s_q) return;
    job_t j = {.type = J_REFRESH, .force = force};
    xQueueSend(s_q, &j, 0);
}

void plat_weather_key_changed(void)
{
    if (!s_q) return;
    job_t j = {.type = J_RESET};
    xQueueSend(s_q, &j, 0);
}

void plat_weather_search(const char *q, wx_search_cb_t cb, void *user)
{
    job_t j = {.type = J_SEARCH, .cb = cb, .user = user};
    snprintf(j.q, sizeof(j.q), "%s", q);
    if (!s_q || xQueueSend(s_q, &j, 0) != pdTRUE) cb(NULL, 0, "Busy - try again", user);
}

int weather_search_sync(const char *q, wx_loc_t *out, int max, char *err, int errn)
{
    if (!s_q) { snprintf(err, errn, "Not ready"); return -1; }
    int n = -1;
    job_t j = {.type = J_SEARCH, .out = out, .max = max, .n = &n, .err = err, .errn = errn};
    snprintf(j.q, sizeof(j.q), "%s", q);
    j.done = xSemaphoreCreateBinary();
    if (xQueueSend(s_q, &j, pdMS_TO_TICKS(1000)) != pdTRUE) {
        vSemaphoreDelete(j.done);
        snprintf(err, errn, "Busy - try again");
        return -1;
    }
    /* The worker finishes a refresh in progress first (a minute at the very worst), then searches.
     * No timeout: the worker writes into this caller's buffers. */
    xSemaphoreTake(j.done, portMAX_DELAY);
    vSemaphoreDelete(j.done);
    return n;
}
