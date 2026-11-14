/*
 * P4Box firmware for the Waveshare ESP32-P4-86-Panel-ETH-2RO (Arduino IDE edition).
 */
#include "board.h"
#include "esp_log.h"
#include "cJSON.h"
#include "weather_net.h"
#include "esp_heap_caps.h"
#include "mbedtls/platform.h"
#include "../../drivers/esp_lvgl_port/esp_lvgl_port.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs_flash.h"
#include "plat_esp.h"
#include "rtc_keep.h"
#include "web.h"
#include "sdcard.h"
#include "../core/platform.h"
#include "../core/settings.h"
#include "../ui/ui.h"

static const char *TAG = "main";

/* The Arduino core builds mbedTLS with internal-RAM buffers.  Radio streams, the speed test and
 * station search can hold several TLS sessions at once, so put them in PSRAM (as the ESP-IDF
 * build of P4Box did with CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC). */
static void *tls_calloc(size_t n, size_t size)
{
    void *p = heap_caps_calloc(n, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_calloc(n, size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static void tls_free(void *p) { heap_caps_free(p); }

static void *json_malloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : heap_caps_malloc(n, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

/* Called from setup() in P4Box.ino. */
void p4box_main(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        nvs_flash_erase();
        nvs_flash_init();
    }
    mbedtls_platform_set_calloc_free(tls_calloc, tls_free);
    /* cJSON trees (weather forecasts are ~30 KB of JSON = thousands of small nodes) go to PSRAM too */
    cJSON_Hooks hooks = {.malloc_fn = json_malloc, .free_fn = heap_caps_free};
    cJSON_InitHooks(&hooks);
    settings_load();
    stations_load();
    board_relays_init();
    plat_time_apply();
    rtc_keep_boot();        /* battery-backed clock: sets the time before anything shows it */
    lock_load();            /* drives the relays back to their last state as early as possible */

    lv_display_t *disp = NULL;
    ESP_ERROR_CHECK(board_display_init(&disp));
    lvgl_port_lock(0);
    ui_init();
    lvgl_port_unlock();

    photos_init();          /* mounts the microSD card in the background */
    net_init();
    time_service_init();
    rtc_keep_start();
    radio_boot();
    weather_init();
    web_start();

    /* Everything came up: keep this image (cancels rollback after an OTA update). */
    esp_ota_mark_app_valid_cancel_rollback();
    ESP_LOGI(TAG, "P4Box %s running", plat_fw_version());
}
