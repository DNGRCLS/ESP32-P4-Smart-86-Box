#pragma once
#include <stddef.h>
#include <stdint.h>
#include "../../lvgl/lvgl.h"

extern const uint8_t ttf_poppins_light[];
extern const size_t ttf_poppins_light_len;
extern const uint8_t ttf_poppins_regular[];
extern const size_t ttf_poppins_regular_len;
extern const uint8_t ttf_poppins_medium[];
extern const size_t ttf_poppins_medium_len;
extern const uint8_t ttf_poppins_bold[];
extern const size_t ttf_poppins_bold_len;

extern const lv_image_dsc_t icon_clock;
extern const lv_image_dsc_t icon_weather;
/* weather conditions, OpenWeather icon codes: _l = 150 px, _s = 60 px */
extern const lv_image_dsc_t wx_01d_l, wx_01d_s;
extern const lv_image_dsc_t wx_01n_l, wx_01n_s;
extern const lv_image_dsc_t wx_02d_l, wx_02d_s;
extern const lv_image_dsc_t wx_02n_l, wx_02n_s;
extern const lv_image_dsc_t wx_03_l, wx_03_s;
extern const lv_image_dsc_t wx_04_l, wx_04_s;
extern const lv_image_dsc_t wx_09_l, wx_09_s;
extern const lv_image_dsc_t wx_10d_l, wx_10d_s;
extern const lv_image_dsc_t wx_10n_l, wx_10n_s;
extern const lv_image_dsc_t wx_11_l, wx_11_s;
extern const lv_image_dsc_t wx_13_l, wx_13_s;
extern const lv_image_dsc_t wx_50_l, wx_50_s;
extern const lv_image_dsc_t icon_radio;
extern const lv_image_dsc_t icon_lock;
extern const lv_image_dsc_t icon_calendar;
extern const lv_image_dsc_t icon_speed;
extern const lv_image_dsc_t icon_photos;
extern const lv_image_dsc_t icon_settings;
extern const lv_image_dsc_t icon_ethernet;
extern const lv_image_dsc_t icon_padlock_small;
extern const lv_image_dsc_t img_padlock_closed;
extern const lv_image_dsc_t img_padlock_open;
