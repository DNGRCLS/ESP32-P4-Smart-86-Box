#pragma once
/*
 * Weather data model, OpenWeather JSON parsers and derived values (humidex, wind chill,
 * heat index, AQHI, US AQI).  Portable C (cJSON only) so it can be tested on a PC.
 *
 * Everything is stored in metric units (°C, m/s, hPa, mm, m); the UI converts for display.
 */
#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WX_LOC_MAX   8
#define WX_HOURS_MAX 24
#define WX_DAYS_MAX  10
#define WX_NA        (-999.0f)      /* value not available */

typedef struct {
    char name[48];
    char region[40];                /* state / province */
    char country[4];
    float lat, lon;
} wx_loc_t;

typedef enum { WX_SRC_NONE = 0, WX_SRC_FREE, WX_SRC_ONECALL3, WX_SRC_ONECALL4 } wx_source_t;

typedef struct {
    time_t dt;
    float temp, feels;
    float pop;                      /* 0..1 */
    float precip_mm;                /* rain + snow in this step */
    char icon[4];                   /* OpenWeather icon code, e.g. "10d" */
} wx_hour_t;

typedef struct {
    time_t dt;                      /* any moment of that local day */
    float tmin, tmax;
    float pop;
    float rain_mm, snow_mm;
    float uvi;                      /* WX_NA when unknown */
    float wind_ms;
    float humidity;
    time_t sunrise, sunset;         /* 0 when unknown */
    float moon_phase;               /* 0..1, WX_NA when unknown */
    char icon[4];
    char desc[40];
} wx_day_t;

typedef struct {
    bool valid;                     /* holds data (possibly old: see fetched / error) */
    float lat, lon;                 /* location this belongs to */
    time_t fetched;
    char error[96];                 /* last refresh problem, "" when fine */
    uint8_t source;                 /* wx_source_t */
    char place[48];                 /* name OpenWeather gave for the coordinates */
    long tz_offset;                 /* seconds east of UTC at the location */

    /* now */
    time_t dt;
    float temp, feels, humidity, pressure, dew_point;
    float wind_ms, gust_ms, wind_deg;
    float visibility_m, clouds, uvi;
    float rain_1h, snow_1h;         /* mm in the last hour */
    int cond_id;
    char icon[4];
    char desc[48];
    time_t sunrise, sunset;

    /* air quality (free Air Pollution API) */
    bool aq_valid;
    int owm_aqi;                    /* 1..5 */
    float pm2_5, pm10, o3, no2, so2, co;   /* µg/m³ */

    /* alert (first one) */
    char alert_event[64];
    char alert_sender[64];
    time_t alert_start, alert_end;
    char alert_desc[400];
    char alert_id[48];              /* One Call 4.0 id */

    int n_hours, hour_step_h;       /* 1 (One Call) or 3 (free forecast) */
    wx_hour_t hours[WX_HOURS_MAX];
    int n_days;
    wx_day_t days[WX_DAYS_MAX];
} weather_t;

/* ---- parsers: return false (and fill err) when the JSON is not what was expected ---- */
bool wx_parse_current25(weather_t *w, const char *json, char *err, int errn);   /* /data/2.5/weather */
bool wx_parse_forecast25(weather_t *w, const char *json, char *err, int errn);  /* /data/2.5/forecast */
bool wx_parse_air(weather_t *w, const char *json);                              /* /data/2.5/air_pollution */
bool wx_parse_onecall3(weather_t *w, const char *json, char *err, int errn);    /* /data/3.0/onecall */
bool wx_parse_oc4_current(weather_t *w, const char *json, char *err, int errn); /* /data/4.0/onecall/current */
bool wx_parse_oc4_hourly(weather_t *w, const char *json);                       /* .../timeline/1h */
bool wx_parse_oc4_daily(weather_t *w, const char *json);                        /* .../timeline/1day */
bool wx_parse_oc4_alert(weather_t *w, const char *json);                        /* .../alert/{id} */
int wx_parse_geocode(const char *json, wx_loc_t *out, int max);                 /* /geo/1.0/direct or /zip */
/* OpenWeather error body {"cod":401,"message":"..."} -> message, or "" */
void wx_api_message(const char *json, char *out, int n);

/* ---- derived values (metric in, metric out) ---- */
float wx_dew_point(float t_c, float rh);
float wx_humidex(float t_c, float dew_c);          /* Environment Canada */
bool wx_humidex_applies(float t_c, float humidex); /* reported at >= 20 °C and humidex >= 25 */
float wx_wind_chill(float t_c, float wind_kmh);    /* Environment Canada / NWS formula */
bool wx_wind_chill_applies(float t_c, float wind_kmh);
float wx_heat_index_c(float t_c, float rh);        /* US NWS (Rothfusz), for imperial users */
bool wx_heat_index_applies(float t_c, float hi_c);
int wx_aqhi(float o3_ugm3, float no2_ugm3, float pm25_ugm3);   /* Canada AQHI, 1..11 (11 = "10+") */
const char *wx_aqhi_risk(int aqhi);
int wx_us_aqi(float pm25, float pm10, float o3_ugm3);         /* US EPA AQI, 0..500 */
const char *wx_us_aqi_label(int aqi);
const char *wx_owm_aqi_label(int aqi);
const char *wx_uv_label(float uvi);
const char *wx_compass(float deg);                 /* "NNE" */
const char *wx_moon_name(float phase);

/* True if a query looks like a postal / ZIP code; fills the zip= value for the geocoding API. */
bool wx_zip_query(const char *q, char *zip, int n);

#ifdef __cplusplus
}
#endif
