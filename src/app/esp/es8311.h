#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define ES8311_ADDR 0x18

esp_err_t es8311_init(i2c_master_bus_handle_t bus, uint8_t addr);
void es8311_set_volume(int percent);
void es8311_mute(bool mute);

#ifdef __cplusplus
}
#endif
