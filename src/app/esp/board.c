/*
 * Board bring-up for the Waveshare ESP32-P4-86-Panel-ETH-2RO.
 * Arduino IDE edition.  Display init follows Waveshare's esp32_p4_wifi6_touch_lcd_4b BSP (ST7703, 2-lane DSI, 480 Mbps).
 */
#include "board.h"
#include "driver/gpio.h"
#include "driver/ledc.h"
#include "esp_check.h"
#include "esp_ldo_regulator.h"
#if __has_include("esp32-hal-ldo.h")
#include "esp32-hal-ldo.h"
#endif
#include "esp_lcd_mipi_dsi.h"
#include "esp_lcd_panel_ops.h"
#include "../../drivers/esp_lcd_st7703/esp_lcd_st7703.h"
#include "../../drivers/esp_lcd_touch/esp_lcd_touch_gt911.h"
#include "esp_log.h"
#include "../../drivers/esp_lvgl_port/esp_lvgl_port.h"

static const char *TAG = "board";

static i2c_master_bus_handle_t s_i2c;
static bool s_relay_state[2];

#define BL_LEDC_TIMER   LEDC_TIMER_1
#define BL_LEDC_CH      LEDC_CHANNEL_1

/* ------------------------------------------------------------------ I2C */

esp_err_t board_i2c_init(void)
{
    if (s_i2c) return ESP_OK;
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = BOARD_I2C_SDA,
        .scl_io_num = BOARD_I2C_SCL,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    return i2c_new_master_bus(&cfg, &s_i2c);
}

i2c_master_bus_handle_t board_i2c(void) { return s_i2c; }

/* ------------------------------------------------------------------ backlight */

static void backlight_init(void)
{
    gpio_config_t en = {.pin_bit_mask = 1ULL << BOARD_LCD_BL_EN, .mode = GPIO_MODE_OUTPUT};
    gpio_config(&en);
    gpio_set_level(BOARD_LCD_BL_EN, 1);

    ledc_timer_config_t t = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_10_BIT,
        .timer_num = BL_LEDC_TIMER,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    ESP_ERROR_CHECK(ledc_timer_config(&t));
    ledc_channel_config_t c = {
        .gpio_num = BOARD_LCD_BL_PWM,
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .channel = BL_LEDC_CH,
        .intr_type = LEDC_INTR_DISABLE,
        .timer_sel = BL_LEDC_TIMER,
        .duty = 0,
        .hpoint = 0,
        .flags.output_invert = 1,   /* per Waveshare BSP */
    };
    ESP_ERROR_CHECK(ledc_channel_config(&c));
}

void board_backlight_set(int percent)
{
    if (percent < 0) percent = 0;
    if (percent > 100) percent = 100;
    /* The backlight driver barely lights below ~10 % duty: map 1..100 % onto 10..100 % duty so
     * the lowest settings stay readable (0 still switches it off). */
    uint32_t duty = percent == 0 ? 0 : 102 + (921 * (uint32_t)percent) / 100;
    ledc_set_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH, duty);
    ledc_update_duty(LEDC_LOW_SPEED_MODE, BL_LEDC_CH);
}

/* ------------------------------------------------------------------ display */

static esp_err_t power_on_ldos(void)
{
#if __has_include("esp32-hal-ldo.h")
    /* Arduino core 3.3.10+: go through the core's LDO manager so its auto-LDO logic stays consistent. */
    static ldo_channel_handle_t vo3, vo4;
    if (!vo3) ESP_RETURN_ON_ERROR(ldoAcquireChannel(3, 2500, false, &vo3), TAG, "LDO3");   /* VDD_MIPI_DPHY */
    if (!vo4) {
        /* VO4 = 3.3 V supply of GPIO 39-48 (SD card, relay 2, RS485) */
        ESP_RETURN_ON_ERROR(ldoAcquireChannel(4, 3300, false, &vo4), TAG, "LDO4");
        ldoDriverClaimChannel(4, "P4Box IO rail");
    }
#else
    static esp_ldo_channel_handle_t vo3, vo4;
    if (!vo3) {
        esp_ldo_channel_config_t phy = {.chan_id = 3, .voltage_mv = 2500};   /* VDD_MIPI_DPHY */
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&phy, &vo3), TAG, "LDO3");
    }
    if (!vo4) {
        esp_ldo_channel_config_t io = {.chan_id = 4, .voltage_mv = 3300};    /* GPIO 39-48 rail */
        ESP_RETURN_ON_ERROR(esp_ldo_acquire_channel(&io, &vo4), TAG, "LDO4");
    }
#endif
    return ESP_OK;
}

static esp_err_t touch_init(esp_lcd_touch_handle_t *out)
{
    esp_lcd_panel_io_i2c_config_t io_cfg = ESP_LCD_TOUCH_IO_I2C_GT911_CONFIG();
    if (i2c_master_probe(s_i2c, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS, 100) == ESP_OK) {
        io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS;
    } else if (i2c_master_probe(s_i2c, ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP, 100) == ESP_OK) {
        io_cfg.dev_addr = ESP_LCD_TOUCH_IO_I2C_GT911_ADDRESS_BACKUP;
    } else {
        ESP_LOGE(TAG, "GT911 touch controller not found");
        return ESP_ERR_NOT_FOUND;
    }
    io_cfg.scl_speed_hz = 400000;
    esp_lcd_panel_io_handle_t io = NULL;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_i2c(s_i2c, &io_cfg, &io), TAG, "touch io");
    esp_lcd_touch_config_t tp = {
        .x_max = BOARD_LCD_H_RES,
        .y_max = BOARD_LCD_V_RES,
        .rst_gpio_num = GPIO_NUM_NC,
        .int_gpio_num = GPIO_NUM_NC,
        .levels = {.reset = 0, .interrupt = 0},
        .flags = {.swap_xy = 0, .mirror_x = 0, .mirror_y = 0},
    };
    ESP_LOGI(TAG, "GT911 at 0x%02x", (unsigned)io_cfg.dev_addr);
    return esp_lcd_touch_new_i2c_gt911(io, &tp, out);
}

esp_err_t board_display_init(lv_display_t **out_disp)
{
    ESP_RETURN_ON_ERROR(board_i2c_init(), TAG, "i2c");
    backlight_init();
    board_backlight_set(0);
    ESP_RETURN_ON_ERROR(power_on_ldos(), TAG, "ldo");

    esp_lcd_dsi_bus_handle_t bus;
    esp_lcd_dsi_bus_config_t bus_cfg = {
        .bus_id = 0,
        .num_data_lanes = 2,
        .phy_clk_src = 0,
        .lane_bit_rate_mbps = 480,
    };
    ESP_RETURN_ON_ERROR(esp_lcd_new_dsi_bus(&bus_cfg, &bus), TAG, "dsi bus");

    esp_lcd_panel_io_handle_t io;
    esp_lcd_dbi_io_config_t dbi = {.virtual_channel = 0, .lcd_cmd_bits = 8, .lcd_param_bits = 8};
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_io_dbi(bus, &dbi, &io), TAG, "dbi");

    esp_lcd_dpi_panel_config_t dpi = ST7703_720_720_PANEL_60HZ_DPI_CONFIG(LCD_COLOR_PIXEL_FORMAT_RGB565);
    dpi.num_fbs = 2;   /* both buffers are handed to LVGL to avoid tearing */
    st7703_vendor_config_t vendor = {
        .flags = {.use_mipi_interface = 1},
        .mipi_config = {.dsi_bus = bus, .dpi_config = &dpi},
    };
    esp_lcd_panel_dev_config_t dev = {
        .bits_per_pixel = 16,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .reset_gpio_num = BOARD_LCD_RST,
        .vendor_config = &vendor,
    };
    esp_lcd_panel_handle_t panel;
    ESP_RETURN_ON_ERROR(esp_lcd_new_panel_st7703(io, &dev, &panel), TAG, "st7703");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "reset");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "init");

    lvgl_port_cfg_t port = ESP_LVGL_PORT_INIT_CONFIG();
    port.task_priority = 4;
    port.task_stack = 24 * 1024;
    port.task_affinity = 1;
    port.task_max_sleep_ms = 50;
    port.timer_period_ms = 5;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&port), TAG, "lvgl port");

    lvgl_port_display_cfg_t dcfg = {
        .io_handle = io,
        .panel_handle = panel,
        .control_handle = NULL,
        .buffer_size = BOARD_LCD_H_RES * BOARD_LCD_V_RES,
        .double_buffer = true,
        .hres = BOARD_LCD_H_RES,
        .vres = BOARD_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT_RGB565,
        .rotation = {.swap_xy = false, .mirror_x = false, .mirror_y = false},
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = true,
        },
    };
    lvgl_port_display_dsi_cfg_t dsi = {.flags = {.avoid_tearing = true}};
    lv_display_t *disp = lvgl_port_add_disp_dsi(&dcfg, &dsi);
    ESP_RETURN_ON_FALSE(disp, ESP_FAIL, TAG, "add display");

    esp_lcd_touch_handle_t tp = NULL;
    if (touch_init(&tp) == ESP_OK) {
        lvgl_port_touch_cfg_t tcfg = {.disp = disp, .handle = tp};
        lvgl_port_add_touch(&tcfg);
    }
    *out_disp = disp;
    ESP_LOGI(TAG, "display ready");
    return ESP_OK;
}

/* ------------------------------------------------------------------ relays */

void board_relays_init(void)
{
    power_on_ldos();   /* relay 2 (GPIO46) sits on the VO4 rail */
    gpio_config_t c = {.pin_bit_mask = (1ULL << BOARD_RELAY1) | (1ULL << BOARD_RELAY2) | (1ULL << BOARD_PA_CTRL),
                       .mode = GPIO_MODE_OUTPUT};
    gpio_config(&c);
    gpio_set_level(BOARD_PA_CTRL, 0);   /* speaker amplifier off until the radio app uses it */
    board_relay_set(1, false);
    board_relay_set(2, false);
}

void board_relay_set(int relay, bool on)
{
    if (relay < 1 || relay > 2) return;
    gpio_set_level(relay == 1 ? BOARD_RELAY1 : BOARD_RELAY2, on ? 1 : 0);
    s_relay_state[relay - 1] = on;
}

bool board_relay_get(int relay)
{
    return (relay >= 1 && relay <= 2) ? s_relay_state[relay - 1] : false;
}
