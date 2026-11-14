#pragma once
#include <stdbool.h>
#include "../core/platform.h"

void radio_init(void);
void radio_play(const char *name, const char *url);
void radio_stop(void);
void radio_set_volume(int percent);
void radio_buzzer(int kind);
void radio_output_usb(bool prefer);   /* use a plugged-in USB audio device (e.g. Bluetooth transmitter) */
void radio_force_speaker(bool on);    /* alarms: keep the built-in speaker whatever is plugged in */
bool radio_output_is_usb(void);   /* alarm tone: 0 off, 1 beeps, 2 buzzer */
void radio_get_status(radio_status_t *st);
void radio_search(const char *query, radio_search_cb_t cb, void *user);
