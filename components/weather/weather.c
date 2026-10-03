// Open-Meteo network calls.
#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "net.h"
#include "weather.h"

static const char *TAG = "weather";

esp_err_t weather_find_place(const char *place, double *lat, double *lon, char *label, size_t label_size) {
    char encoded[192], url[320];
    net_url_encode(place, encoded, sizeof encoded);
    snprintf(url, sizeof url, "https://geocoding-api.open-meteo.com/v1/search?name=%s&count=1&language=en&format=json", encoded);
    char *body = NULL;
    int status = 0;
    esp_err_t err = net_https("GET", url, NULL, NULL, NULL, &body, &status);
    if (err == ESP_OK && status != 200) err = ESP_FAIL;
    if (err == ESP_OK && !weather_parse_place(body, lat, lon, label, label_size)) err = ESP_ERR_NOT_FOUND;
    free(body);
    if (err == ESP_OK) ESP_LOGI(TAG, "\"%s\" -> %s (%.4f, %.4f)", place, label, *lat, *lon);
    else ESP_LOGW(TAG, "place lookup for \"%s\" failed: %s", place, esp_err_to_name(err));
    return err;
}

esp_err_t weather_fetch(double lat, double lon, bool fahrenheit, const char *timezone, weather_report_t *out) {
    char tz[96], url[512];
    net_url_encode(timezone, tz, sizeof tz);
    snprintf(url, sizeof url,
             "https://api.open-meteo.com/v1/forecast?latitude=%.4f&longitude=%.4f&current=temperature_2m,weather_code"
             "&daily=weather_code,temperature_2m_max,temperature_2m_min,precipitation_probability_max"
             "&temperature_unit=%s&timezone=%s&forecast_days=16",
             lat, lon, fahrenheit ? "fahrenheit" : "celsius", tz);
    char *body = NULL;
    int status = 0;
    esp_err_t err = net_https("GET", url, NULL, NULL, NULL, &body, &status);
    if (err == ESP_OK && (status != 200 || !weather_parse_forecast(body, fahrenheit, out))) err = ESP_FAIL;
    free(body);
    if (err != ESP_OK) ESP_LOGW(TAG, "forecast failed: %s (HTTP %d)", esp_err_to_name(err), status);
    return err;
}
