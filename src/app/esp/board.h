#pragma once
#include <stdbool.h>
#include "esp_err.h"
#include "driver/i2c_master.h"
#include "../../lvgl/lvgl.h"

/* Waveshare ESP32-P4-86-Panel-ETH-2RO pin map (main PCB + 86 bottom board) */
#define BOARD_I2C_SDA        7
#define BOARD_I2C_SCL        8
#define BOARD_LCD_RST        27
#define BOARD_LCD_BL_PWM     26
#define BOARD_LCD_BL_EN      33
#define BOARD_RELAY1         32
#define BOARD_RELAY2         46
#define BOARD_RS485_TX       47
#define BOARD_RS485_RX       48
#define BOARD_PA_CTRL        53

#define BOARD_LCD_H_RES      720
#define BOARD_LCD_V_RES      720

esp_err_t board_i2c_init(void);
i2c_master_bus_handle_t board_i2c(void);

/* Brings up LDOs, MIPI-DSI ST7703 panel, GT911 touch and the LVGL port. */
esp_err_t board_display_init(lv_display_t **out_disp);
void board_backlight_set(int percent);

/* Relays: outputs driven low (off) until the lock app restores its state. */
void board_relays_init(void);
void board_relay_set(int relay, bool on);
bool board_relay_get(int relay);
