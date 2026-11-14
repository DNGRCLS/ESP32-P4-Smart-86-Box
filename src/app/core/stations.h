#pragma once
#include <stdbool.h>

#define STATIONS_MAX 48

typedef struct {
    char name[64];
    char url[256];
} station_t;

typedef struct {
    int count;
    station_t items[STATIONS_MAX];
} stations_t;

extern stations_t g_stations;

void stations_set_defaults(stations_t *s);
char *stations_to_json(const stations_t *s);           /* caller frees */
bool stations_from_json(stations_t *s, const char *json);
int stations_find_url(const stations_t *s, const char *url);
bool stations_add(stations_t *s, const char *name, const char *url);
void stations_delete(stations_t *s, int idx);
void stations_move(stations_t *s, int idx, int dir);   /* dir -1 up, +1 down */
bool stations_url_valid(const char *url);
