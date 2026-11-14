/*
 * USB audio output (USB Audio Class 1.0) on the panel's USB-C OTG port.
 *
 * Meant for USB Bluetooth audio transmitters such as the Avantree C81: the dongle shows up as a
 * USB sound card and does the Bluetooth pairing itself, so the panel just streams stereo PCM to it.
 * Wired USB headphones / DACs that follow USB Audio 1.0 work the same way.
 *
 *   usb_lib task  : USB host library (root port = the high-speed OTG port)
 *   uac driver    : Espressif usb_host_uac, with its own background task
 *   usb_aud task  : connect / disconnect events
 *   radio decoder : usb_audio_start() / usb_audio_write() / usb_audio_stop()
 *
 * Note: devices must be plugged in directly. Espressif's host stack cannot reach full-speed devices
 * (which these dongles are) through a hub on the high-speed port.
 */
#include "usb_audio.h"
#include <stdio.h>
#include <string.h>
#include "esp_check.h"
#include "esp_intr_alloc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "usb/usb_host.h"
#include "../../drivers/usb_host_uac/include/usb/uac_host.h"

static const char *TAG = "usb_audio";

#define USB_BUF_BYTES   (24 * 1024)     /* ~125 ms of 48 kHz stereo 16-bit */
#define USB_BUF_THRESH  (6 * 1024)

typedef struct {
    bool driver;                         /* true: driver event, false: device event */
    uint8_t addr, iface;
    int event;
    uac_host_device_handle_t handle;
} evt_t;

static QueueHandle_t s_evq;
static SemaphoreHandle_t s_mtx;
static uac_host_device_handle_t s_dev;   /* opened speaker interface, NULL when none */
static bool s_running;                   /* stream started */
static uint32_t s_rate;                  /* rate the stream runs at */
static char s_name[48];
static uint32_t s_rates[12];
static int s_nrates;
static uint32_t s_rate_lo, s_rate_hi;    /* continuous range, 0 when discrete */
static uint8_t s_bits = 16;
static volatile uint32_t s_gen;          /* bumps on connect / disconnect */
static volatile bool s_writing;          /* the decoder is inside uac_host_device_write() */

#define LOCK() xSemaphoreTake(s_mtx, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_mtx)

/* ------------------------------------------------------------------ callbacks -> queue */

static void driver_cb(uint8_t addr, uint8_t iface_num, const uac_host_driver_event_t event, void *arg)
{
    (void)arg;
    evt_t e = {.driver = true, .addr = addr, .iface = iface_num, .event = event};
    xQueueSend(s_evq, &e, 0);
}

static void device_cb(uac_host_device_handle_t h, const uac_host_device_event_t event, void *arg)
{
    (void)arg;
    if (event != UAC_HOST_DRIVER_EVENT_DISCONNECTED && event != UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) return;
    evt_t e = {.driver = false, .event = event, .handle = h};
    xQueueSend(s_evq, &e, 0);
}

/* ------------------------------------------------------------------ connect */

static void wstr_to_ascii(const wchar_t *w, char *out, size_t n)
{
    size_t o = 0;
    for (size_t i = 0; w[i] && o + 1 < n; i++) out[o++] = (w[i] >= 32 && w[i] < 127) ? (char)w[i] : '?';
    out[o] = 0;
}

static void on_speaker(uint8_t addr, uint8_t iface)
{
    LOCK();
    bool busy = s_dev != NULL;
    UNLOCK();
    if (busy) { ESP_LOGW(TAG, "a second USB speaker was ignored"); return; }

    const uac_host_device_config_t cfg = {
        .addr = addr,
        .iface_num = iface,
        .buffer_size = USB_BUF_BYTES,
        .buffer_threshold = USB_BUF_THRESH,
        .callback = device_cb,
        .callback_arg = NULL,
    };
    uac_host_device_handle_t h = NULL;
    if (uac_host_device_open(&cfg, &h) != ESP_OK) { ESP_LOGE(TAG, "could not open the USB speaker"); return; }

    uac_host_dev_info_t info;
    char name[48] = "USB audio";
    if (uac_host_get_device_info(h, &info) == ESP_OK) {
        wstr_to_ascii(info.iProduct, name, sizeof(name));
        if (!name[0]) snprintf(name, sizeof(name), "USB audio %04X:%04X", info.VID, info.PID);
        ESP_LOGI(TAG, "connected: %s (%04X:%04X), %d alt settings", name, info.VID, info.PID, info.iface_alt_num);
    }

    /* collect what the device can take as 2-channel 16-bit (else 24-bit) PCM */
    uint32_t rates[12];
    int nr = 0;
    uint32_t lo = 0, hi = 0;
    uint8_t bits = 0;
    for (int pass = 0; pass < 2 && !bits; pass++) {
        uint8_t want_bits = pass == 0 ? 16 : 24;
        for (uint8_t alt = 1; alt <= info.iface_alt_num; alt++) {
            uac_host_dev_alt_param_t p;
            if (uac_host_get_device_alt_param(h, alt, &p) != ESP_OK) continue;
            ESP_LOGI(TAG, "  alt %d: %d ch, %d bit, %s", alt, p.channels, p.bit_resolution,
                     p.sample_freq_type ? "fixed rates" : "rate range");
            if (p.channels != 2 || p.bit_resolution != want_bits) continue;
            bits = want_bits;
            if (p.sample_freq_type) {
                for (int i = 0; i < p.sample_freq_type && nr < 12; i++) rates[nr++] = p.sample_freq[i];
            } else {
                lo = p.sample_freq_lower;
                hi = p.sample_freq_upper;
            }
        }
    }
    if (!bits) {
        ESP_LOGW(TAG, "%s has no 2-channel 16/24-bit output: not used", name);
        uac_host_device_close(h);
        return;
    }
    LOCK();
    s_dev = h;
    s_running = false;
    s_bits = bits;
    memcpy(s_rates, rates, sizeof(rates));
    s_nrates = nr;
    s_rate_lo = lo;
    s_rate_hi = hi;
    snprintf(s_name, sizeof(s_name), "%s", name);
    s_gen++;
    UNLOCK();
    for (int i = 0; i < nr; i++) ESP_LOGI(TAG, "  %u Hz", (unsigned)rates[i]);
}

static void on_gone(uac_host_device_handle_t h)
{
    LOCK();
    bool ours = h == s_dev;
    if (ours) {
        s_dev = NULL;
        s_running = false;
        s_gen++;
    }
    UNLOCK();
    /* let a write that is in progress run out (it fails quickly once the device is gone) */
    for (int i = 0; i < 100 && s_writing; i++) vTaskDelay(pdMS_TO_TICKS(10));
    uac_host_device_close(h);
    if (ours) ESP_LOGI(TAG, "USB audio device removed");
}

/* ------------------------------------------------------------------ tasks */

static void usb_lib_task(void *arg)
{
    const usb_host_config_t cfg = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LOWMED,
        /* peripheral_map 0 = the high-speed controller, wired to the panel's USB-C OTG port */
    };
    esp_err_t err = usb_host_install(&cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "USB host install failed: %s", esp_err_to_name(err));
        xTaskNotifyGive((TaskHandle_t)arg);
        vTaskDelete(NULL);
        return;
    }
    xTaskNotifyGive((TaskHandle_t)arg);
    for (;;) {
        uint32_t flags;
        usb_host_lib_handle_events(portMAX_DELAY, &flags);
        if (flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) usb_host_device_free_all();
    }
}

static void event_task(void *arg)
{
    (void)arg;
    TaskHandle_t me = xTaskGetCurrentTaskHandle();
    xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, me, 6, NULL, 0);
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
    const uac_host_driver_config_t ucfg = {
        .create_background_task = true,
        .task_priority = 6,
        .stack_size = 4096,
        .core_id = 0,
        .callback = driver_cb,
        .callback_arg = NULL,
    };
    if (uac_host_install(&ucfg) != ESP_OK) {
        ESP_LOGE(TAG, "USB audio driver install failed");
        vTaskDelete(NULL);
        return;
    }
    ESP_LOGI(TAG, "ready: plug a USB audio device (e.g. a Bluetooth transmitter) into the USB-C OTG port");
    for (;;) {
        evt_t e;
        if (xQueueReceive(s_evq, &e, portMAX_DELAY) != pdTRUE) continue;
        if (e.driver) {
            if (e.event == UAC_HOST_DRIVER_EVENT_TX_CONNECTED) on_speaker(e.addr, e.iface);
            /* RX (microphone) interfaces are ignored */
        } else if (e.event == UAC_HOST_DRIVER_EVENT_DISCONNECTED) {
            on_gone(e.handle);
        } else if (e.event == UAC_HOST_DEVICE_EVENT_TRANSFER_ERROR) {
            ESP_LOGW(TAG, "USB audio transfer error");
        }
    }
}

/* ------------------------------------------------------------------ public */

void usb_audio_init(void)
{
    s_mtx = xSemaphoreCreateMutex();
    s_evq = xQueueCreate(8, sizeof(evt_t));
    /* Talks to the USB stack only: no flash writes, but keep it in internal RAM like the driver's tasks. */
    xTaskCreatePinnedToCore(event_task, "usb_aud", 4096, NULL, 5, NULL, 0);
}

bool usb_audio_connected(void)
{
    if (!s_mtx) return false;
    LOCK();
    bool c = s_dev != NULL;
    UNLOCK();
    return c;
}

uint32_t usb_audio_gen(void) { return s_gen; }

void usb_audio_name(char *out, size_t n)
{
    if (!s_mtx) { if (n) out[0] = 0; return; }
    LOCK();
    snprintf(out, n, "%s", s_dev ? s_name : "");
    UNLOCK();
}

uint32_t usb_audio_rate(void) { return s_running ? s_rate : 0; }

/* Best device rate for a stream: the same rate if offered, else 48 kHz, 44.1 kHz, or the closest. */
static uint32_t pick_rate(uint32_t want)
{
    if (s_nrates == 0) {
        if (want >= s_rate_lo && want <= s_rate_hi) return want;
        if (48000 >= s_rate_lo && 48000 <= s_rate_hi) return 48000;
        return want < s_rate_lo ? s_rate_lo : s_rate_hi;
    }
    for (int i = 0; i < s_nrates; i++) if (s_rates[i] == want) return want;
    for (int i = 0; i < s_nrates; i++) if (s_rates[i] == 48000) return 48000;
    for (int i = 0; i < s_nrates; i++) if (s_rates[i] == 44100) return 44100;
    uint32_t best = s_rates[0];
    for (int i = 1; i < s_nrates; i++) {
        uint32_t d = s_rates[i] > want ? s_rates[i] - want : want - s_rates[i];
        uint32_t bd = best > want ? best - want : want - best;
        if (d < bd) best = s_rates[i];
    }
    return best;
}

uint32_t usb_audio_start(uint32_t stream_rate)
{
    if (!s_mtx) return 0;
    LOCK();
    uac_host_device_handle_t h = s_dev;
    if (!h) { UNLOCK(); return 0; }
    uint32_t rate = pick_rate(stream_rate);
    if (s_running && rate == s_rate) { UNLOCK(); return rate; }
    if (s_running) { uac_host_device_stop(h); s_running = false; }
    const uac_host_stream_config_t sc = {.channels = 2, .bit_resolution = s_bits, .sample_freq = rate, .flags = 0};
    esp_err_t err = uac_host_device_start(h, &sc);
    if (err == ESP_OK) {
        s_running = true;
        s_rate = rate;
        uac_host_device_set_mute(h, false);       /* not every device has these controls */
        uac_host_device_set_volume(h, 100);       /* volume is applied to the samples instead */
        ESP_LOGI(TAG, "streaming %u Hz %u-bit stereo to %s (source %u Hz)", (unsigned)rate, s_bits, s_name, (unsigned)stream_rate);
    } else {
        ESP_LOGE(TAG, "start at %u Hz failed: %s", (unsigned)rate, esp_err_to_name(err));
        rate = 0;
    }
    UNLOCK();
    return rate;
}

void usb_audio_stop(void)
{
    if (!s_mtx) return;
    LOCK();
    if (s_dev && s_running) uac_host_device_stop(s_dev);
    s_running = false;
    UNLOCK();
}

/* frames of interleaved stereo s16; converted to 24-bit if that is what the device takes */
bool usb_audio_write(const int16_t *pcm, uint32_t frames, uint32_t timeout_ms)
{
    LOCK();
    uac_host_device_handle_t h = s_running ? s_dev : NULL;
    uint8_t bits = s_bits;
    s_writing = h != NULL;
    UNLOCK();
    if (!h) return false;
    esp_err_t err;
    if (bits == 16) {
        err = uac_host_device_write(h, (uint8_t *)pcm, frames * 4, pdMS_TO_TICKS(timeout_ms));
    } else {
        static uint8_t buf[480 * 2 * 3];
        err = ESP_OK;
        uint32_t done = 0;
        while (done < frames && err == ESP_OK) {
            uint32_t n = frames - done > 480 ? 480 : frames - done;
            for (uint32_t i = 0; i < n * 2; i++) {
                int16_t v = pcm[done * 2 + i];
                buf[i * 3] = 0;
                buf[i * 3 + 1] = (uint8_t)(v & 0xFF);
                buf[i * 3 + 2] = (uint8_t)((v >> 8) & 0xFF);
            }
            err = uac_host_device_write(h, buf, n * 6, pdMS_TO_TICKS(timeout_ms));
            done += n;
        }
    }
    s_writing = false;
    return err == ESP_OK;
}
