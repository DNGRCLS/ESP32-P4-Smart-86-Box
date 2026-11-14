/*
 * Network manager.
 *  - Ethernet (IP101, RMII) with a higher route priority, so it carries traffic whenever
 *    a cable is connected and has an address; Wi-Fi takes over otherwise.
 *  - Wi-Fi station through the ESP32-C6 co-processor (esp_wifi_remote + ESP-Hosted).
 *  - Setup hotspot ("captive portal") started automatically when no network comes up.
 *  - mDNS: http://<hostname>.local
 */
#include "net.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <time.h>
#include "captive_dns.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_hosted.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_system.h"
#include "esp32-hal-hosted.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_sntp.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "mdns.h"
#include "../core/settings.h"

static const char *TAG = "net";

static esp_netif_t *s_eth_netif, *s_sta_netif, *s_ap_netif;
static esp_eth_handle_t s_eth;
static SemaphoreHandle_t s_mtx;
static SemaphoreHandle_t s_scan_mtx;

static net_status_t s_st;
static bool s_wifi_started;
static bool s_portal_auto;
static int64_t s_retry_at_us;
static int s_retry_step;
static int64_t s_online_since_us;
static bool s_scanning;
static char s_c6_ver[24] = "unknown";
static c6fw_t s_c6fw;

#define LOCK() xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

static const int s_backoff_s[] = {2, 5, 10, 20, 30, 60};

/* ------------------------------------------------------------------ helpers */

static void ip_to_str(const esp_netif_ip_info_t *ip, char *out)
{
    snprintf(out, 16, IPSTR, IP2STR(&ip->ip));
}

static void update_active_locked(void)
{
    esp_netif_t *def = esp_netif_get_default_netif();
    if (def && def == s_eth_netif && s_st.eth_has_ip) s_st.active = NET_ACTIVE_ETH;
    else if (s_st.wifi_connected) s_st.active = NET_ACTIVE_WIFI;
    else if (s_st.eth_has_ip) s_st.active = NET_ACTIVE_ETH;
    else s_st.active = NET_ACTIVE_NONE;
}

static void on_network_up(void)
{
    /* Ask SNTP for time right away instead of waiting for its next poll. */
    if (esp_sntp_enabled()) esp_sntp_restart();
}

/* ------------------------------------------------------------------ events */

static void eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    LOCK();
    if (id == ETHERNET_EVENT_CONNECTED) {
        s_st.eth_link = true;
        ESP_LOGI(TAG, "Ethernet link up");
    } else if (id == ETHERNET_EVENT_DISCONNECTED) {
        s_st.eth_link = false;
        s_st.eth_has_ip = false;
        s_st.eth_ip[0] = 0;
        ESP_LOGI(TAG, "Ethernet link down");
    }
    update_active_locked();
    UNLOCK();
}

static void wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    LOCK();
    switch (id) {
    case WIFI_EVENT_STA_START:
        ESP_LOGI(TAG, "STA started");
        break;
    case WIFI_EVENT_STA_CONNECTED:
        ESP_LOGI(TAG, "STA associated");
        break;
    case WIFI_EVENT_STA_DISCONNECTED: {
        wifi_event_sta_disconnected_t *d = (wifi_event_sta_disconnected_t *)data;
        ESP_LOGW(TAG, "STA disconnected, reason %d", d ? d->reason : -1);
        s_st.wifi_connected = false;
        s_st.wifi_connecting = false;
        s_st.wifi_ip[0] = 0;
        int step = s_retry_step < (int)(sizeof(s_backoff_s) / sizeof(s_backoff_s[0])) ? s_retry_step
                   : (int)(sizeof(s_backoff_s) / sizeof(s_backoff_s[0])) - 1;
        s_retry_at_us = esp_timer_get_time() + (int64_t)s_backoff_s[step] * 1000000;
        s_retry_step++;
        break;
    }
    case WIFI_EVENT_AP_STACONNECTED:
        ESP_LOGI(TAG, "device joined setup hotspot");
        break;
    default:
        break;
    }
    update_active_locked();
    UNLOCK();
}

static void ip_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    bool up = false;
    LOCK();
    if (id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ip_to_str(&e->ip_info, s_st.wifi_ip);
        s_st.wifi_connected = true;
        s_st.wifi_connecting = false;
        s_retry_step = 0;
        ESP_LOGI(TAG, "Wi-Fi IP %s", s_st.wifi_ip);
        up = true;
    } else if (id == IP_EVENT_STA_LOST_IP) {
        s_st.wifi_ip[0] = 0;
    } else if (id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *e = (ip_event_got_ip_t *)data;
        ip_to_str(&e->ip_info, s_st.eth_ip);
        s_st.eth_has_ip = true;
        ESP_LOGI(TAG, "Ethernet IP %s", s_st.eth_ip);
        up = true;
    } else if (id == IP_EVENT_ETH_LOST_IP) {
        s_st.eth_has_ip = false;
        s_st.eth_ip[0] = 0;
    }
    update_active_locked();
    UNLOCK();
    if (up) on_network_up();
}

/* ------------------------------------------------------------------ ethernet */

static void eth_start(void)
{
    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();   /* P4 defaults match this board */
    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = 1;
    phy_cfg.reset_gpio_num = 51;
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_cfg);
    if (!mac || !phy) {
        ESP_LOGE(TAG, "Ethernet driver creation failed");
        return;
    }
    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    if (esp_eth_driver_install(&cfg, &s_eth) != ESP_OK) {
        ESP_LOGE(TAG, "Ethernet install failed");
        return;
    }
    esp_netif_inherent_config_t base = ESP_NETIF_INHERENT_DEFAULT_ETH();
    base.route_prio = 200;   /* above Wi-Fi STA (100): Ethernet wins when both are up */
    esp_netif_config_t ncfg = {.base = &base, .stack = ESP_NETIF_NETSTACK_DEFAULT_ETH};
    s_eth_netif = esp_netif_new(&ncfg);
    esp_netif_attach(s_eth_netif, esp_eth_new_netif_glue(s_eth));
    esp_netif_set_hostname(s_eth_netif, g_settings.hostname);
    esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, eth_event, NULL);
    esp_eth_start(s_eth);
}

/* ------------------------------------------------------------------ wi-fi */

static void wifi_apply_sta_config(const char *ssid, const char *pass)
{
    wifi_config_t c = {0};
    snprintf((char *)c.sta.ssid, sizeof(c.sta.ssid), "%s", ssid);
    snprintf((char *)c.sta.password, sizeof(c.sta.password), "%s", pass);
    c.sta.threshold.authmode = pass[0] ? WIFI_AUTH_WEP : WIFI_AUTH_OPEN;
    c.sta.pmf_cfg.capable = true;
    c.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    c.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    esp_wifi_set_config(WIFI_IF_STA, &c);
}

static void wifi_start(void)
{
    /* Arduino core: bring up the SDIO link to the C6 with the board's pins before using Wi-Fi. */
    if (!hostedInitWiFi()) {
        ESP_LOGE(TAG, "ESP-Hosted link to the C6 co-processor failed");
        return;
    }
    LOCK();
    s_c6fw.link_ok = true;
    {
        uint32_t hm, hn, hp;
        hostedGetHostVersion(&hm, &hn, &hp);
        snprintf(s_c6fw.expected, sizeof(s_c6fw.expected), "%u.%u.%u", (unsigned)hm, (unsigned)hn, (unsigned)hp);
    }
    UNLOCK();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta_netif, g_settings.hostname);
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_err_t err = esp_wifi_init(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_init failed: %s (is the C6 co-processor responding?)", esp_err_to_name(err));
        return;
    }
    esp_wifi_set_storage(WIFI_STORAGE_RAM);
    esp_wifi_set_mode(WIFI_MODE_STA);
    esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, wifi_event, NULL);
    if (esp_wifi_start() != ESP_OK) {
        ESP_LOGE(TAG, "esp_wifi_start failed");
        return;
    }
    s_wifi_started = true;

    esp_hosted_coprocessor_fwver_t v = {0};
    if (esp_hosted_get_coprocessor_fwversion(&v) == ESP_OK) {
        snprintf(s_c6_ver, sizeof(s_c6_ver), "%u.%u.%u", (unsigned)v.major1, (unsigned)v.minor1, (unsigned)v.patch1);
        ESP_LOGI(TAG, "C6 co-processor firmware %s", s_c6_ver);
        uint32_t hm, hn, hp;
        hostedGetHostVersion(&hm, &hn, &hp);
        LOCK();
        s_c6fw.known = true;
        snprintf(s_c6fw.installed, sizeof(s_c6fw.installed), "%s", s_c6_ver);
        snprintf(s_c6fw.expected, sizeof(s_c6fw.expected), "%u.%u.%u", (unsigned)hm, (unsigned)hn, (unsigned)hp);
        UNLOCK();
        bool upd = hostedHasUpdate();
        LOCK();
        s_c6fw.update_available = upd;
        UNLOCK();
        if (upd) ESP_LOGW(TAG, "C6 firmware %s is older than the host's %s: update it from Settings > System", s_c6_ver, s_c6fw.expected);
    }

    if (g_settings.wifi_ssid[0]) {
        LOCK();
        s_st.wifi_configured = true;
        s_st.wifi_connecting = true;
        snprintf(s_st.wifi_ssid, sizeof(s_st.wifi_ssid), "%s", g_settings.wifi_ssid);
        UNLOCK();
        wifi_apply_sta_config(g_settings.wifi_ssid, g_settings.wifi_pass);
        esp_wifi_connect();
    }
}

void net_wifi_connect(const char *ssid, const char *pass)
{
    snprintf(g_settings.wifi_ssid, sizeof(g_settings.wifi_ssid), "%s", ssid);
    snprintf(g_settings.wifi_pass, sizeof(g_settings.wifi_pass), "%s", pass ? pass : "");
    if (!s_wifi_started) return;
    LOCK();
    s_st.wifi_configured = ssid[0] != 0;
    s_st.wifi_connecting = ssid[0] != 0;
    s_st.wifi_connected = false;
    s_retry_step = 0;
    s_retry_at_us = 0;
    snprintf(s_st.wifi_ssid, sizeof(s_st.wifi_ssid), "%s", ssid);
    UNLOCK();
    esp_wifi_disconnect();
    wifi_apply_sta_config(g_settings.wifi_ssid, g_settings.wifi_pass);
    if (ssid[0]) esp_wifi_connect();
}

void net_wifi_forget(void)
{
    g_settings.wifi_ssid[0] = 0;
    g_settings.wifi_pass[0] = 0;
    LOCK();
    s_st.wifi_configured = false;
    s_st.wifi_connecting = false;
    s_st.wifi_ssid[0] = 0;
    UNLOCK();
    if (s_wifi_started) {
        esp_wifi_disconnect();
        wifi_apply_sta_config("", "");
    }
}

static int cmp_rssi(const void *a, const void *b)
{
    return ((const wifi_scan_rec_t *)b)->rssi - ((const wifi_scan_rec_t *)a)->rssi;
}

int net_wifi_scan(wifi_scan_rec_t *out, int max)
{
    if (!s_wifi_started) return 0;
    xSemaphoreTake(s_scan_mtx, portMAX_DELAY);
    s_scanning = true;
    wifi_scan_config_t sc = {.show_hidden = false, .scan_type = WIFI_SCAN_TYPE_ACTIVE};
    esp_err_t err = esp_wifi_scan_start(&sc, true);
    if (err != ESP_OK) {
        /* A connection attempt in progress blocks scanning: stop it and retry once. */
        esp_wifi_disconnect();
        vTaskDelay(pdMS_TO_TICKS(300));
        err = esp_wifi_scan_start(&sc, true);
    }
    int n = 0;
    if (err == ESP_OK) {
        uint16_t num = 32;
        wifi_ap_record_t *recs = calloc(num, sizeof(wifi_ap_record_t));
        if (recs && esp_wifi_scan_get_ap_records(&num, recs) == ESP_OK) {
            for (int i = 0; i < num && n < max; i++) {
                const char *ssid = (const char *)recs[i].ssid;
                if (!ssid[0]) continue;
                bool dup = false;
                for (int j = 0; j < n; j++) {
                    if (strcmp(out[j].ssid, ssid) == 0) {
                        if (recs[i].rssi > out[j].rssi) out[j].rssi = recs[i].rssi;
                        dup = true;
                        break;
                    }
                }
                if (dup) continue;
                snprintf(out[n].ssid, sizeof(out[n].ssid), "%s", ssid);
                out[n].rssi = recs[i].rssi;
                out[n].secure = recs[i].authmode != WIFI_AUTH_OPEN;
                n++;
            }
        }
        free(recs);
        qsort(out, n, sizeof(out[0]), cmp_rssi);
    } else {
        ESP_LOGW(TAG, "scan failed: %s", esp_err_to_name(err));
    }
    s_scanning = false;
    LOCK();
    if (s_st.wifi_configured && !s_st.wifi_connected) s_retry_at_us = esp_timer_get_time() + 1000000;
    UNLOCK();
    xSemaphoreGive(s_scan_mtx);
    return n;
}

/* ------------------------------------------------------------------ setup hotspot */

void net_portal_start(bool automatic)
{
    if (!s_wifi_started) return;
    LOCK();
    bool already = s_st.portal_active;
    UNLOCK();
    if (already) return;
    if (!s_ap_netif) s_ap_netif = esp_netif_create_default_wifi_ap();
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_BASE);
    wifi_config_t ap = {0};
    snprintf((char *)ap.ap.ssid, sizeof(ap.ap.ssid), "P4Box-Setup-%02X%02X", mac[4], mac[5]);
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    ap.ap.channel = 6;
    ap.ap.authmode = WIFI_AUTH_OPEN;
    ap.ap.max_connection = 4;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    captive_dns_start();
    LOCK();
    s_st.portal_active = true;
    s_portal_auto = automatic;
    snprintf(s_st.portal_ssid, sizeof(s_st.portal_ssid), "%s", (char *)ap.ap.ssid);
    snprintf(s_st.portal_ip, sizeof(s_st.portal_ip), "192.168.4.1");
    UNLOCK();
    ESP_LOGI(TAG, "setup hotspot %s started", (char *)ap.ap.ssid);
}

void net_portal_stop(void)
{
    LOCK();
    bool active = s_st.portal_active;
    s_st.portal_active = false;
    UNLOCK();
    if (!active) return;
    captive_dns_stop();
    esp_wifi_set_mode(WIFI_MODE_STA);
    ESP_LOGI(TAG, "setup hotspot stopped");
}

/* ------------------------------------------------------------------ supervisor */

static void supervise_once(void)
{
    int64_t now = esp_timer_get_time();
    bool do_connect = false, start_portal = false, stop_portal = false;
    LOCK();
    bool online = s_st.eth_has_ip || s_st.wifi_connected;
    if (online) {
        if (!s_online_since_us) s_online_since_us = now;
    } else {
        s_online_since_us = 0;
    }
    /* Reconnect with backoff.  While the hotspot is up, retry less often so phones stay connected. */
    if (s_wifi_started && s_st.wifi_configured && !s_st.wifi_connected && !s_scanning && s_retry_at_us &&
        now >= s_retry_at_us) {
        do_connect = true;
        s_st.wifi_connecting = true;
        s_retry_at_us = 0;
        if (s_st.portal_active) s_retry_at_us = now + 60LL * 1000000;
    }
    /* Auto hotspot: nothing online 30 s after boot. */
    if (!online && !s_st.portal_active && now > 30LL * 1000000) start_portal = true;
    /* Auto-started hotspot closes once we've been online for 2 minutes. */
    if (s_st.portal_active && s_portal_auto && s_online_since_us && now - s_online_since_us > 120LL * 1000000)
        stop_portal = true;
    UNLOCK();

    LOCK();
    bool sta_up = s_st.wifi_connected;
    UNLOCK();
    if (s_wifi_started && sta_up) {
        wifi_ap_record_t ap;
        if (esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            LOCK();
            s_st.wifi_rssi = ap.rssi;
            UNLOCK();
        }
    }
    if (do_connect) esp_wifi_connect();
    if (start_portal) net_portal_start(true);
    if (stop_portal) net_portal_stop();
}

/* Runs in its own task: Wi-Fi calls are RPCs to the C6 and may block. */
static void supervise_task(void *arg)
{
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(2000));
        supervise_once();
    }
}

/* ------------------------------------------------------------------ public */

void net_get_status(net_status_t *st)
{
    if (!s_mtx) {
        memset(st, 0, sizeof(*st));
        return;
    }
    LOCK();
    update_active_locked();
    *st = s_st;
    UNLOCK();
}

void net_set_time_synced(void)
{
    LOCK();
    s_st.time_synced = true;
    s_st.last_sync = time(NULL);
    UNLOCK();
}

const char *net_c6_version(void) { return s_c6_ver; }

/* ------------------------------------------------------------------ C6 firmware update */

static void c6_msg(const char *m, int pct, bool done, bool failed)
{
    LOCK();
    snprintf(s_c6fw.msg, sizeof(s_c6fw.msg), "%s", m);
    if (pct >= 0) s_c6fw.pct = pct;
    s_c6fw.done = done;
    s_c6fw.failed = failed;
    if (done || failed) s_c6fw.running = false;
    UNLOCK();
}

/* ---- streaming writer shared by the online update and the web-page upload ---- */

/*
 * Each esp_hosted_slave_ota_write() is sent to the C6 as ONE RPC message.  Newer host stacks
 * split a big message into 1536-byte SDIO fragments, but older C6 firmware (the kind that is
 * "too old to report a version") cannot put fragments back together: every write still answers
 * OK, yet the image the C6 stores is garbage and it rejects it at the end.  So the data is cut
 * into pieces that each fit in a single SDIO frame, the same 1400 bytes Espressif's own
 * host_performs_slave_ota example uses.
 */
#define C6_OTA_PIECE 1400

static bool s_c6_ota_open;
static uint32_t s_c6_ota_bytes, s_c6_ota_pieces;
static char s_c6_ota_err[96];

const char *net_c6_ota_error(void) { return s_c6_ota_err; }

void net_c6_ota_abort(void)
{
    if (s_c6_ota_open) hostedEndUpdate();
    s_c6_ota_open = false;
}

bool net_c6_ota_begin(void)
{
    s_c6_ota_err[0] = 0;
    s_c6_ota_bytes = s_c6_ota_pieces = 0;
    if (!s_c6fw.link_ok) {
        snprintf(s_c6_ota_err, sizeof(s_c6_ota_err), "The Wi-Fi module link is down");
        return false;
    }
    s_c6_ota_open = hostedBeginUpdate();
    if (!s_c6_ota_open) snprintf(s_c6_ota_err, sizeof(s_c6_ota_err), "The Wi-Fi module refused to start the update");
    else ESP_LOGI(TAG, "C6 OTA: started");
    return s_c6_ota_open;
}

bool net_c6_ota_write(const uint8_t *buf, uint32_t len)
{
    if (!s_c6_ota_open) return false;
    if (s_c6_ota_bytes == 0 && len >= 16) {
        /* ESP app image header: magic 0xE9, chip id at offset 12 (ESP32-C6 = 13). */
        uint16_t chip = buf[12] | (buf[13] << 8);
        if (buf[0] != 0xE9 || chip != 13) {
            snprintf(s_c6_ota_err, sizeof(s_c6_ota_err), buf[0] != 0xE9 ? "Not an ESP firmware file" : "That firmware is not for an ESP32-C6 (chip id %u)", chip);
            ESP_LOGE(TAG, "C6 OTA: %s", s_c6_ota_err);
            s_c6_ota_open = false;
            hostedEndUpdate(); /* abandon the half-open update on the C6 */
            return false;
        }
    }
    while (len) {
        uint32_t n = len > C6_OTA_PIECE ? C6_OTA_PIECE : len;
        if (!hostedWriteUpdate((uint8_t *)buf, n)) {
            snprintf(s_c6_ota_err, sizeof(s_c6_ota_err), "Write failed at byte %u (piece %u)", (unsigned)s_c6_ota_bytes, (unsigned)s_c6_ota_pieces);
            ESP_LOGE(TAG, "C6 OTA: %s", s_c6_ota_err);
            s_c6_ota_open = false;
            return false;
        }
        buf += n;
        len -= n;
        s_c6_ota_bytes += n;
        s_c6_ota_pieces++;
        if ((s_c6_ota_pieces % 100) == 0) ESP_LOGI(TAG, "C6 OTA: %u bytes written", (unsigned)s_c6_ota_bytes);
    }
    return true;
}

bool net_c6_ota_finish(void)
{
    if (!s_c6_ota_open) return false;
    s_c6_ota_open = false;
    ESP_LOGI(TAG, "C6 OTA: %u bytes in %u pieces, finalising", (unsigned)s_c6_ota_bytes, (unsigned)s_c6_ota_pieces);
    if (!hostedEndUpdate()) {
        snprintf(s_c6_ota_err, sizeof(s_c6_ota_err), "The Wi-Fi module rejected the image after %u bytes", (unsigned)s_c6_ota_bytes);
        ESP_LOGE(TAG, "C6 OTA: %s", s_c6_ota_err);
        return false;
    }
    /* Firmware older than 2.6 has no "activate" call and switches over by itself on its next
     * reset; the Arduino helper already treats that case as success. */
    if (!hostedActivateUpdate()) {
        snprintf(s_c6_ota_err, sizeof(s_c6_ota_err), "The new firmware was stored but could not be activated");
        ESP_LOGE(TAG, "C6 OTA: %s", s_c6_ota_err);
        return false;
    }
    ESP_LOGI(TAG, "C6 OTA: done");
    return true;
}

void net_c6_ota_progress(int pct, const char *msg)
{
    LOCK();
    s_c6fw.running = true;
    s_c6fw.pct = pct;
    snprintf(s_c6fw.msg, sizeof(s_c6fw.msg), "%s", msg);
    UNLOCK();
}

void net_c6_ota_result(bool ok, const char *msg) { c6_msg(msg, ok ? 100 : -1, ok, !ok); }

/* Opens url and returns the client positioned at the body, or NULL with err filled in. */
static esp_http_client_handle_t c6_open(const char *url, int64_t *len, char *err, size_t errn)
{
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 1024,
        .user_agent = "P4Box/1.0 (ESP32-P4)",
        .max_redirection_count = 5,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) { snprintf(err, errn, "out of memory"); return NULL; }
    for (int redir = 0; redir < 5; redir++) {
        esp_err_t e = esp_http_client_open(c, 0);
        if (e != ESP_OK) {
            snprintf(err, errn, "connect failed (%s)", esp_err_to_name(e));
            break;
        }
        *len = esp_http_client_fetch_headers(c);
        int st = esp_http_client_get_status_code(c);
        if (st == 301 || st == 302 || st == 303 || st == 307 || st == 308) {
            esp_http_client_set_redirection(c);
            esp_http_client_close(c);
            continue;
        }
        if (st == 200 && *len > 100000) return c;
        snprintf(err, errn, "HTTP %d, %lld bytes", st, (long long)*len);
        esp_http_client_close(c);
        break;
    }
    esp_http_client_cleanup(c);
    return NULL;
}

static void c6_update_task(void *arg)
{
    char urls[2][160];
    const char *u0 = hostedGetUpdateURL();     /* https://espressif.github.io/arduino-esp32/hosted/<chip>-v<x.y.z>.bin */
    snprintf(urls[0], sizeof(urls[0]), "%s", u0);
    const char *file = strrchr(u0, '/');
    snprintf(urls[1], sizeof(urls[1]), "https://raw.githubusercontent.com/espressif/arduino-esp32/gh-pages/hosted%s", file ? file : "/");
    esp_http_client_handle_t c = NULL;
    int64_t len = 0;
    char err[2][64] = {"", ""};
    for (int i = 0; i < 2 && !c; i++) {
        ESP_LOGI(TAG, "C6 update: trying %s", urls[i]);
        c = c6_open(urls[i], &len, err[i], sizeof(err[i]));
        if (!c) ESP_LOGW(TAG, "C6 update: %s -> %s", urls[i], err[i]);
    }
    if (!c) {
        char m[80];
        snprintf(m, sizeof(m), "Download failed: %s", err[1][0] ? err[1] : err[0]);
        c6_msg(m, -1, false, true);
        vTaskDelete(NULL);
        return;
    }
    uint8_t *buf = heap_caps_malloc(C6_OTA_PIECE, MALLOC_CAP_SPIRAM);
    bool ok = buf && net_c6_ota_begin();
    if (!ok) c6_msg(buf ? net_c6_ota_error() : "Out of memory", -1, false, true);
    int64_t got = 0;
    while (ok && got < len) {
        int r = esp_http_client_read(c, (char *)buf, C6_OTA_PIECE);
        if (r <= 0) { ok = false; net_c6_ota_abort(); c6_msg("Download interrupted", -1, false, true); break; }
        if (!net_c6_ota_write(buf, r)) { ok = false; c6_msg(net_c6_ota_error(), -1, false, true); break; }
        got += r;
        c6_msg("Writing to the Wi-Fi module...", (int)(got * 100 / len), false, false);
    }
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    heap_caps_free(buf);
    if (ok && !net_c6_ota_finish()) { ok = false; c6_msg(net_c6_ota_error(), -1, false, true); }
    if (ok) {
        c6_msg("Done - restarting...", 100, true, false);
        vTaskDelay(pdMS_TO_TICKS(1500));
        esp_restart();
    }
    vTaskDelete(NULL);
}

void plat_c6_fw_status(c6fw_t *st)
{
    if (!s_mtx) { memset(st, 0, sizeof(*st)); return; }
    LOCK();
    *st = s_c6fw;
    UNLOCK();
}

void plat_c6_fw_update(void)
{
    LOCK();
    bool busy = s_c6fw.running;
    if (!busy) {
        s_c6fw.running = true;
        s_c6fw.failed = s_c6fw.done = false;
        s_c6fw.pct = 0;
        snprintf(s_c6fw.msg, sizeof(s_c6fw.msg), "Downloading...");
    }
    UNLOCK();
    if (!busy && xTaskCreate(c6_update_task, "c6_ota", 8192, NULL, 4, NULL) != pdPASS) c6_msg("Out of memory", -1, false, true);
}

void net_hostname_changed(void)
{
    if (s_eth_netif) esp_netif_set_hostname(s_eth_netif, g_settings.hostname);
    if (s_sta_netif) esp_netif_set_hostname(s_sta_netif, g_settings.hostname);
    mdns_hostname_set(g_settings.hostname);
}

void net_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_scan_mtx = xSemaphoreCreateMutex();
    if (time(NULL) > 1735689600) {   /* clock kept across reset / by the RTC battery: 2025-01-01 */
        s_st.time_synced = true;
        s_st.last_sync = 0;
    }
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t e = esp_event_loop_create_default();
    if (e != ESP_OK && e != ESP_ERR_INVALID_STATE) ESP_ERROR_CHECK(e);
    esp_event_handler_register(IP_EVENT, ESP_EVENT_ANY_ID, ip_event, NULL);

    eth_start();
    wifi_start();

    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(g_settings.hostname);
        mdns_instance_name_set("P4Box panel");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    }

    xTaskCreate(supervise_task, "net_sup", 4096, NULL, 3, NULL);
}
