/*
 * P4Box - multi-app firmware for the Waveshare ESP32-P4-86-Panel-ETH-2RO
 * (4" 720x720 touch panel, Ethernet, Wi-Fi via ESP32-C6, 2 relays, speaker, microSD).
 *
 * Apps: Clock, Web Radio, Door Lock (relays), Calendar (Canadian holidays), Speed Test,
 *       Photos (JPEG viewer / photo frame from the microSD card), Settings.
 *
 * Arduino IDE settings (Tools menu) - see README.md for details:
 *   Board            : ESP32P4 Dev Module          (esp32 by Espressif Systems 3.3.x)
 *   Chip Variant     : match your chip (the boot log shows "chip revision: v1.x" or "v3.x")
 *   Flash Size       : 32MB (256Mb)
 *   Partition Scheme : 32M Flash (13MB APP/6.75MB SPIFFS)
 *   PSRAM            : Enabled
 *
 * All the code lives in src/ (LVGL 9.3 is bundled there, no libraries to install).
 */
#include <Arduino.h>
#include "esp_log.h"

#ifndef BOARD_HAS_PSRAM
#error "P4Box needs PSRAM: set Tools > PSRAM > Enabled"
#endif
#if ESP_ARDUINO_VERSION < ESP_ARDUINO_VERSION_VAL(3, 3, 4)
#error "Update the esp32 board package (Tools > Board > Boards Manager) to 3.3.4 or newer - 3.3.12 is tested"
#endif
#if !defined(CONFIG_IDF_TARGET_ESP32P4)
#error "Select Tools > Board > ESP32 Arduino > ESP32P4 Dev Module"
#endif

extern "C" void p4box_main(void);

// setup() builds the whole UI and starts the services: give it more than the default 8 KB.
/* The USB-C OTG port is used as a USB host for USB / Bluetooth audio transmitters. */
#if ARDUINO_USB_CDC_ON_BOOT && !ARDUINO_USB_MODE
#error "Set Tools > USB CDC On Boot: Disabled (P4Box uses the USB-C OTG port for USB audio; the serial log is on the USB-UART port)"
#endif

SET_LOOP_TASK_STACK_SIZE(24 * 1024);

void setup()
{
    Serial.begin(115200);              // boot log on the USB-UART port
    esp_log_level_set("*", ESP_LOG_INFO);
    esp_log_level_set("lv_tiny_ttf", ESP_LOG_NONE);
    p4box_main();
}

void loop()
{
    // Everything runs in FreeRTOS tasks started by p4box_main(): end this task to give its
    // 24 KB stack back to internal RAM.
    vTaskDelete(NULL);
}
