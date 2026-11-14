/*
 * Network speed test against Cloudflare (speed.cloudflare.com).
 *   ping/jitter : 12 small requests on one keep-alive TLS connection (first one discarded)
 *   download    : 4 parallel streams of /__down for ~10 s
 *   upload      : 3 parallel streams POSTing to /__up for ~10 s
 * Traffic follows the default route, i.e. Ethernet when it is up, Wi-Fi otherwise.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs.h"
#include "../core/platform.h"

static const char *TAG = "speed";

#define HOST          "https://speed.cloudflare.com"
#define DL_STREAMS    4
#define UL_STREAMS    3
#define PHASE_US      (10LL * 1000000)
#define HIST_MAX      10

static SemaphoreHandle_t s_mtx;
static speedtest_t s_st;
static volatile bool s_cancel, s_running;
static volatile bool s_phase_stop;
static volatile int64_t s_bytes;     /* bytes moved in the current phase (all streams) */
static volatile int s_workers;
static speed_hist_t s_hist[HIST_MAX];
static int s_hist_n = -1;

#define LOCK() xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

static void add_bytes(int n)
{
    LOCK();
    s_bytes += n;
    UNLOCK();
}

static esp_http_client_handle_t client(const char *url, esp_http_client_method_t m)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .method = m,
        .timeout_ms = 8000,
        .buffer_size = 16384,
        .buffer_size_tx = 4096,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .keep_alive_enable = true,
        .user_agent = "P4Box/0.3 speedtest",
    };
    return esp_http_client_init(&cfg);
}

/* ------------------------------------------------------------------ history */

static void hist_load(void)
{
    if (s_hist_n >= 0) return;
    s_hist_n = 0;
    nvs_handle_t h;
    if (nvs_open("p4box", NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_hist);
        if (nvs_get_blob(h, "speedhist", s_hist, &len) == ESP_OK) s_hist_n = len / sizeof(speed_hist_t);
        nvs_close(h);
    }
}

static void hist_add(const speedtest_t *r)
{
    hist_load();
    memmove(&s_hist[1], &s_hist[0], sizeof(speed_hist_t) * (HIST_MAX - 1));
    s_hist[0] = (speed_hist_t){.t = time(NULL), .ping_ms = r->ping_ms, .jitter_ms = r->jitter_ms,
                               .down_mbps = r->down_mbps, .up_mbps = r->up_mbps};
    snprintf(s_hist[0].iface, sizeof(s_hist[0].iface), "%s", r->iface);
    if (s_hist_n < HIST_MAX) s_hist_n++;
    nvs_handle_t h;
    if (nvs_open("p4box", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "speedhist", s_hist, sizeof(speed_hist_t) * s_hist_n);
        nvs_commit(h);
        nvs_close(h);
    }
}

/* ------------------------------------------------------------------ phases */

static bool fetch_trace(void)
{
    esp_http_client_handle_t c = client(HOST "/cdn-cgi/trace", HTTP_METHOD_GET);
    if (!c) return false;
    bool ok = false;
    if (esp_http_client_open(c, 0) == ESP_OK && esp_http_client_fetch_headers(c) >= 0 &&
        esp_http_client_get_status_code(c) == 200) {
        char buf[1024];
        int n = 0, r;
        while (n < (int)sizeof(buf) - 1 && (r = esp_http_client_read(c, buf + n, sizeof(buf) - 1 - n)) > 0) n += r;
        buf[n] = 0;
        char *p;
        LOCK();
        if ((p = strstr(buf, "ip="))) sscanf(p + 3, "%47[^\n]", s_st.ip);
        if ((p = strstr(buf, "colo="))) sscanf(p + 5, "%39[^\n]", s_st.server);
        UNLOCK();
        ok = true;
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return ok;
}

static bool measure_ping(void)
{
    esp_http_client_handle_t c = client(HOST "/__down?bytes=0", HTTP_METHOD_GET);
    if (!c) return false;
    float rtt[12];
    int n = 0;
    for (int i = 0; i < 12 && !s_cancel; i++) {
        int64_t t0 = esp_timer_get_time();
        esp_err_t e = esp_http_client_perform(c);
        int64_t t1 = esp_timer_get_time();
        if (e != ESP_OK || esp_http_client_get_status_code(c) != 200) continue;
        if (i == 0) continue;   /* first request includes the TLS handshake */
        rtt[n++] = (t1 - t0) / 1000.0f;
        LOCK();
        float mn = rtt[0];
        for (int k = 1; k < n; k++) if (rtt[k] < mn) mn = rtt[k];
        s_st.ping_ms = mn;
        s_st.progress_pct = i * 100 / 12;
        UNLOCK();
    }
    esp_http_client_cleanup(c);
    if (n < 3) return false;
    /* ping = median, jitter = mean difference between consecutive samples */
    float sorted[12];
    memcpy(sorted, rtt, sizeof(float) * n);
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            if (sorted[j] < sorted[i]) { float t = sorted[i]; sorted[i] = sorted[j]; sorted[j] = t; }
    float jit = 0;
    for (int i = 1; i < n; i++) jit += fabsf(rtt[i] - rtt[i - 1]);
    LOCK();
    s_st.ping_ms = sorted[n / 2];
    s_st.jitter_ms = jit / (n - 1);
    UNLOCK();
    return true;
}

static void dl_worker(void *arg)
{
    char *buf = heap_caps_malloc(16384, MALLOC_CAP_SPIRAM);
    esp_http_client_handle_t c = client(HOST "/__down?bytes=100000000", HTTP_METHOD_GET);
    while (buf && c && !s_phase_stop && !s_cancel) {
        if (esp_http_client_open(c, 0) != ESP_OK) break;
        esp_http_client_fetch_headers(c);
        int r;
        while (!s_phase_stop && !s_cancel && (r = esp_http_client_read(c, buf, 16384)) > 0) add_bytes(r);
        esp_http_client_close(c);
    }
    if (c) esp_http_client_cleanup(c);
    free(buf);
    LOCK();
    s_workers--;
    UNLOCK();
    vTaskDelete(NULL);
}

static void ul_worker(void *arg)
{
    const int chunk = 16384;
    const int req_len = 25 * 1024 * 1024;
    char *buf = heap_caps_malloc(chunk, MALLOC_CAP_SPIRAM);
    if (buf) esp_fill_random(buf, chunk);
    esp_http_client_handle_t c = client(HOST "/__up", HTTP_METHOD_POST);
    if (c) esp_http_client_set_header(c, "Content-Type", "application/octet-stream");
    while (buf && c && !s_phase_stop && !s_cancel) {
        if (esp_http_client_open(c, req_len) != ESP_OK) break;
        int sent = 0;
        while (sent < req_len && !s_phase_stop && !s_cancel) {
            int w = esp_http_client_write(c, buf, chunk < req_len - sent ? chunk : req_len - sent);
            if (w <= 0) break;
            sent += w;
            add_bytes(w);
        }
        if (sent >= req_len) esp_http_client_fetch_headers(c);
        esp_http_client_close(c);
    }
    if (c) esp_http_client_cleanup(c);
    free(buf);
    LOCK();
    s_workers--;
    UNLOCK();
    vTaskDelete(NULL);
}

/* Runs one throughput phase; returns Mbps measured after a 1.5 s warm-up. */
static float run_phase(spd_phase_t phase, TaskFunction_t worker, int streams)
{
    LOCK();
    s_st.phase = phase;
    s_st.live_mbps = 0;
    s_st.progress_pct = 0;
    s_bytes = 0;
    s_workers = streams;
    UNLOCK();
    s_phase_stop = false;
    for (int i = 0; i < streams; i++) xTaskCreate(worker, "spd_w", 10240, NULL, 5, NULL);

    int64_t t0 = esp_timer_get_time(), warm_t = 0;
    int64_t warm_b = 0, last_b = 0, last_t = t0;
    float ema = 0;
    while (!s_cancel) {
        vTaskDelay(pdMS_TO_TICKS(250));
        int64_t now = esp_timer_get_time();
        LOCK();
        int64_t b = s_bytes;
        int w = s_workers;
        UNLOCK();
        float inst = (b - last_b) * 8.0f / ((now - last_t) / 1e6f) / 1e6f;
        ema = ema == 0 ? inst : ema * 0.7f + inst * 0.3f;
        last_b = b;
        last_t = now;
        if (!warm_t && now - t0 > 1500000) { warm_t = now; warm_b = b; }
        LOCK();
        s_st.live_mbps = ema;
        s_st.progress_pct = (int)((now - t0) * 100 / PHASE_US);
        UNLOCK();
        if (now - t0 >= PHASE_US || w == 0) break;
    }
    s_phase_stop = true;
    int64_t end_t = esp_timer_get_time();
    LOCK();
    int64_t end_b = s_bytes;
    UNLOCK();
    for (int i = 0; i < 100 && s_workers > 0; i++) vTaskDelay(pdMS_TO_TICKS(50));
    if (!warm_t || end_t <= warm_t) return 0;
    return (end_b - warm_b) * 8.0f / ((end_t - warm_t) / 1e6f) / 1e6f;
}

static void fail(const char *msg)
{
    LOCK();
    s_st.phase = s_cancel ? SPD_IDLE : SPD_ERROR;
    snprintf(s_st.error, sizeof(s_st.error), "%s", msg);
    UNLOCK();
}

static void test_task(void *arg)
{
    net_status_t ns;
    net_get_status(&ns);
    LOCK();
    memset(&s_st, 0, sizeof(s_st));
    s_st.phase = SPD_PREPARE;
    snprintf(s_st.iface, sizeof(s_st.iface), "%s", ns.active == NET_ACTIVE_ETH ? "Ethernet" : ns.active == NET_ACTIVE_WIFI ? "Wi-Fi" : "-");
    UNLOCK();

    if (ns.active == NET_ACTIVE_NONE) { fail("No network connection"); goto out; }
    if (!fetch_trace()) { fail("Cannot reach speed.cloudflare.com"); goto out; }
    LOCK();
    s_st.phase = SPD_PING;
    UNLOCK();
    if (!measure_ping()) { fail("Ping measurement failed"); goto out; }
    if (s_cancel) goto out;

    float dn = run_phase(SPD_DOWNLOAD, dl_worker, DL_STREAMS);
    LOCK();
    s_st.down_mbps = dn;
    UNLOCK();
    if (s_cancel) goto out;
    float up = run_phase(SPD_UPLOAD, ul_worker, UL_STREAMS);
    LOCK();
    s_st.up_mbps = up;
    s_st.live_mbps = 0;
    s_st.phase = (dn > 0 || up > 0) ? SPD_DONE : SPD_ERROR;
    if (s_st.phase == SPD_ERROR) snprintf(s_st.error, sizeof(s_st.error), "Transfers failed");
    speedtest_t copy = s_st;
    UNLOCK();
    if (copy.phase == SPD_DONE) {
        LOCK();
        hist_add(&copy);
        UNLOCK();
    }
    ESP_LOGI(TAG, "ping %.1f ms jitter %.1f ms down %.1f up %.1f Mbps via %s", copy.ping_ms, copy.jitter_ms,
             copy.down_mbps, copy.up_mbps, copy.iface);
out:
    if (s_cancel) {
        LOCK();
        s_st.phase = SPD_IDLE;
        UNLOCK();
    }
    s_running = false;
    vTaskDelete(NULL);
}

/* ------------------------------------------------------------------ platform */

void plat_speedtest_start(void)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    if (s_running) return;
    s_running = true;
    s_cancel = false;
    if (xTaskCreate(test_task, "speedtest", 10240, NULL, 4, NULL) != pdPASS) s_running = false;
}

void plat_speedtest_cancel(void) { s_cancel = true; }

void plat_speedtest_get(speedtest_t *st)
{
    if (!s_mtx) { memset(st, 0, sizeof(*st)); return; }
    LOCK();
    *st = s_st;
    UNLOCK();
}

int plat_speed_history(speed_hist_t *out, int max)
{
    if (!s_mtx) s_mtx = xSemaphoreCreateMutex();
    LOCK();
    hist_load();
    int n = s_hist_n < max ? s_hist_n : max;
    memcpy(out, s_hist, sizeof(speed_hist_t) * n);
    UNLOCK();
    return n;
}
