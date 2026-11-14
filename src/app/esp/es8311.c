/*
 * Minimal ES8311 DAC driver (playback only) for the panel's speaker output.
 * The codec runs as I2S slave with MCLK = 256 x Fs, so one clock setup covers every
 * sample rate the radio streams use (8 - 48 kHz).
 */
#include "es8311.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "es8311";
static i2c_master_dev_handle_t s_dev;

static esp_err_t wr(uint8_t reg, uint8_t val)
{
    uint8_t b[2] = {reg, val};
    return i2c_master_transmit(s_dev, b, 2, 100);
}

static int rd(uint8_t reg)
{
    uint8_t v = 0;
    if (i2c_master_transmit_receive(s_dev, &reg, 1, &v, 1, 100) != ESP_OK) return -1;
    return v;
}

esp_err_t es8311_init(i2c_master_bus_handle_t bus, uint8_t addr)
{
    if (!s_dev) {
        i2c_device_config_t dc = {.dev_addr_length = I2C_ADDR_BIT_LEN_7, .device_address = addr, .scl_speed_hz = 100000};
        esp_err_t e = i2c_master_bus_add_device(bus, &dc, &s_dev);
        if (e != ESP_OK) return e;
    }
    int id1 = rd(0xFD), id2 = rd(0xFE);
    if (id1 < 0) {
        ESP_LOGE(TAG, "codec not answering at 0x%02x", addr);
        return ESP_ERR_NOT_FOUND;
    }
    ESP_LOGI(TAG, "chip id %02x%02x", id1, id2);

    /* reset, then power-on state machine (same order as Espressif's es8311 driver) */
    wr(0x00, 0x1F);
    vTaskDelay(pdMS_TO_TICKS(20));
    wr(0x00, 0x00);
    wr(0x00, 0x80);           /* power on, slave mode */
    wr(0x45, 0x00);
    wr(0x01, 0x3F);           /* all clocks on, MCLK from the MCLK pin */
    wr(0x02, 0x00);           /* pre-divider 1, multiplier 1  (MCLK = 256 Fs) */
    wr(0x03, 0x10);           /* ADC: single speed, OSR 16 */
    wr(0x04, 0x10);           /* DAC OSR 16 */
    wr(0x05, 0x00);           /* ADC/DAC clock dividers 1 */
    wr(0x06, 0x03);           /* BCLK divider (unused as slave) */
    wr(0x07, 0x00);
    wr(0x08, 0xFF);           /* LRCK = MCLK / 256 */
    wr(0x09, 0x0C);           /* DAC serial port: I2S, 16 bit */
    wr(0x0A, 0x0C);           /* ADC serial port: I2S, 16 bit */
    wr(0x0B, 0x00);
    wr(0x0C, 0x00);
    wr(0x10, 0x1F);           /* analog references */
    wr(0x11, 0x7F);
    wr(0x13, 0x10);
    wr(0x1B, 0x0A);
    wr(0x1C, 0x6A);
    wr(0x0D, 0x01);           /* power up analog circuits */
    wr(0x0E, 0x02);
    wr(0x12, 0x00);           /* power up DAC */
    wr(0x14, 0x1A);
    wr(0x15, 0x40);
    wr(0x37, 0x08);           /* bypass DAC equaliser */
    wr(0x31, 0x00);           /* unmute */
    es8311_set_volume(60);
    return ESP_OK;
}

void es8311_set_volume(int percent)
{
    if (!s_dev) return;
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* 0x32: 0x00 = -95.5 dB ... 0xBF = 0 dB, 0.5 dB steps.  Use -48 .. -2 dB (the NS4150B
     * amplifier runs from 5 V, so full codec level would clip the small speaker). */
    int reg;
    if (percent == 0) reg = 0;
    else reg = 0xBF - 4 - (100 - percent) * 92 / 99;
    wr(0x32, (uint8_t)reg);
}

void es8311_mute(bool mute)
{
    if (!s_dev) return;
    wr(0x31, mute ? 0x60 : 0x00);
}
