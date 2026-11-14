#pragma once
#include "../../lvgl/lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Call once with the LVGL lock held, after the display is registered. */
void ui_init(void);

/* Call (LVGL lock held) after settings were changed from outside the UI, e.g. the web page. */
void ui_on_external_settings_change(void);

/* Alarm control for the web page (LVGL lock held). */
#include <stdbool.h>
#include <stddef.h>
void ui_alarm_status(bool *ringing, char *label, size_t ln, char *next, size_t nn);
void ui_alarm_stop(void);
void ui_alarm_snooze(void);

#ifdef __cplusplus
}
#endif
