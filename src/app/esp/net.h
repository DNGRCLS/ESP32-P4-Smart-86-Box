#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "../core/platform.h"

void net_init(void);
void net_get_status(net_status_t *st);
void net_wifi_connect(const char *ssid, const char *pass);
void net_wifi_forget(void);
/* Blocking scan (up to ~4 s). Returns number of records written. */
int net_wifi_scan(wifi_scan_rec_t *out, int max);
void net_portal_start(bool automatic);
void net_portal_stop(void);
void net_set_time_synced(void);
void net_hostname_changed(void);
const char *net_c6_version(void);

/* ESP32-C6 firmware update, also used by the web page's "upload Wi-Fi module firmware". */
bool net_c6_ota_begin(void);
bool net_c6_ota_write(const uint8_t *buf, uint32_t len);
bool net_c6_ota_finish(void);           /* end + activate */
void net_c6_ota_abort(void);
const char *net_c6_ota_error(void);   /* reason for the last failure */
void net_c6_ota_progress(int pct, const char *msg);
void net_c6_ota_result(bool ok, const char *msg);
