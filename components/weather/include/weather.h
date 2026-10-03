// Weather from Open-Meteo (no API key), and place lookup for the setup page's city/ZIP field.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

#define WEATHER_MAX_DAYS 16

typedef struct {
    int y, m, d;
    float high, low;
    int precip_chance;  // %, -1 if unknown
    int code;           // WMO weather code
} weather_day_t;

typedef struct {
    bool valid;
    float temperature;
    int code;
    char unit;  // 'F' or 'C'
    weather_day_t days[WEATHER_MAX_DAYS];
    int day_count;
} weather_report_t;

// Short description for a WMO weather code, e.g. "Light rain".
const char *weather_describe(int code);

// Parse an Open-Meteo forecast response.
bool weather_parse_forecast(const char *json, bool fahrenheit, weather_report_t *out);

// Fetch the forecast (current + 16 days) for a location, in the given IANA timezone.
esp_err_t weather_fetch(double lat, double lon, bool fahrenheit, const char *timezone, weather_report_t *out);

// Parse an Open-Meteo geocoding response; label gets e.g. "Seattle, Washington, United States".
bool weather_parse_place(const char *json, double *lat, double *lon, char *label, size_t label_size);

// Look up a city or postal code online. ESP_ERR_NOT_FOUND if there's no match.
esp_err_t weather_find_place(const char *place, double *lat, double *lon, char *label, size_t label_size);
