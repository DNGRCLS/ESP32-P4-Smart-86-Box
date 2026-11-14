#pragma once
#include "../core/weather.h"

void weather_init(void);
/* Blocking place search for the web page (runs on the weather task). Returns the count or -1. */
int weather_search_sync(const char *q, wx_loc_t *out, int max, char *err, int errn);
