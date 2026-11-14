#include "stations.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cJSON.h"

stations_t g_stations;

/* Built-in list.  Long-running public streams; the user can edit or delete any of them. */
static const station_t s_defaults[] = {
    {"Kool FM 107.5 Barrie (CKMB)", "https://cob.leanstream.co/CKMBFM-MP3"},
    {"Radio Paradise - Main Mix", "http://stream.radioparadise.com/mp3-128"},
    {"Radio Paradise - Mellow", "http://stream.radioparadise.com/mellow-128"},
    {"Radio Paradise - Rock", "http://stream.radioparadise.com/rock-128"},
    {"SomaFM Groove Salad", "https://ice2.somafm.com/groovesalad-128-mp3"},
    {"SomaFM Indie Pop Rocks", "https://ice2.somafm.com/indiepop-128-mp3"},
    {"SomaFM Christmas Lounge", "https://ice2.somafm.com/christmas-128-mp3"},
    {"KEXP 90.3 Seattle", "https://kexp-mp3-128.streamguys1.com/kexp128.mp3"},
    {"Radio Swiss Classic", "https://stream.srg-ssr.ch/m/rsc_de/mp3_128"},
    {"Radio Swiss Jazz", "https://stream.srg-ssr.ch/m/rsj/mp3_128"},
    {"Radio Swiss Pop", "https://stream.srg-ssr.ch/m/rsp/mp3_128"},
    {"BBC World Service", "http://stream.live.vc.bbcmedia.co.uk/bbc_world_service"},
};

void stations_set_defaults(stations_t *s)
{
    memset(s, 0, sizeof(*s));
    for (size_t i = 0; i < sizeof(s_defaults) / sizeof(s_defaults[0]) && i < STATIONS_MAX; i++) {
        s->items[s->count++] = s_defaults[i];
    }
}

bool stations_url_valid(const char *url)
{
    if (!url) return false;
    if (strncmp(url, "http://", 7) != 0 && strncmp(url, "https://", 8) != 0) return false;
    return strlen(url) > 10 && strlen(url) < sizeof(((station_t *)0)->url);
}

char *stations_to_json(const stations_t *s)
{
    cJSON *arr = cJSON_CreateArray();
    for (int i = 0; i < s->count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "name", s->items[i].name);
        cJSON_AddStringToObject(o, "url", s->items[i].url);
        cJSON_AddItemToArray(arr, o);
    }
    char *out = cJSON_PrintUnformatted(arr);
    cJSON_Delete(arr);
    return out;
}

bool stations_from_json(stations_t *s, const char *json)
{
    cJSON *arr = cJSON_Parse(json);
    if (!cJSON_IsArray(arr)) {
        cJSON_Delete(arr);
        return false;
    }
    /* ~15 KB: keep it off the caller's stack */
    stations_t *tmpp = calloc(1, sizeof(stations_t));
    if (!tmpp) {
        cJSON_Delete(arr);
        return false;
    }
#define tmp (*tmpp)
    cJSON *o;
    cJSON_ArrayForEach(o, arr) {
        if (tmp.count >= STATIONS_MAX) break;
        cJSON *n = cJSON_GetObjectItem(o, "name");
        cJSON *u = cJSON_GetObjectItem(o, "url");
        if (!cJSON_IsString(u) || !stations_url_valid(u->valuestring)) continue;
        station_t *st = &tmp.items[tmp.count++];
        snprintf(st->url, sizeof(st->url), "%s", u->valuestring);
        snprintf(st->name, sizeof(st->name), "%s",
                 (cJSON_IsString(n) && n->valuestring[0]) ? n->valuestring : u->valuestring);
    }
    cJSON_Delete(arr);
    *s = tmp;
#undef tmp
    free(tmpp);
    return true;
}

int stations_find_url(const stations_t *s, const char *url)
{
    for (int i = 0; i < s->count; i++)
        if (strcmp(s->items[i].url, url) == 0) return i;
    return -1;
}

bool stations_add(stations_t *s, const char *name, const char *url)
{
    if (s->count >= STATIONS_MAX || !stations_url_valid(url)) return false;
    station_t *st = &s->items[s->count++];
    snprintf(st->url, sizeof(st->url), "%s", url);
    snprintf(st->name, sizeof(st->name), "%s", (name && name[0]) ? name : url);
    return true;
}

void stations_delete(stations_t *s, int idx)
{
    if (idx < 0 || idx >= s->count) return;
    memmove(&s->items[idx], &s->items[idx + 1], (size_t)(s->count - idx - 1) * sizeof(station_t));
    s->count--;
}

void stations_move(stations_t *s, int idx, int dir)
{
    int j = idx + dir;
    if (idx < 0 || idx >= s->count || j < 0 || j >= s->count) return;
    station_t t = s->items[idx];
    s->items[idx] = s->items[j];
    s->items[j] = t;
}
