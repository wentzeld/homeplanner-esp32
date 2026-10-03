// Open-Meteo response parsing (pure C + cJSON; host-tested).
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "weather.h"

bool weather_parse_place(const char *json, double *lat, double *lon, char *label, size_t label_size) {
    cJSON *root = cJSON_Parse(json ? json : "");
    const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(root, "results"), 0);
    const cJSON *la = cJSON_GetObjectItemCaseSensitive(first, "latitude");
    const cJSON *lo = cJSON_GetObjectItemCaseSensitive(first, "longitude");
    bool ok = cJSON_IsNumber(la) && cJSON_IsNumber(lo);
    if (ok) {
        *lat = la->valuedouble;
        *lon = lo->valuedouble;
        const char *parts[3] = {NULL};
        const char *keys[3] = {"name", "admin1", "country"};
        for (int i = 0; i < 3; i++) {
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(first, keys[i]);
            if (cJSON_IsString(v) && v->valuestring[0]) parts[i] = v->valuestring;
        }
        size_t n = 0;
        label[0] = '\0';
        for (int i = 0; i < 3; i++) {
            if (!parts[i] || (i > 0 && parts[i - 1] && strcmp(parts[i], parts[i - 1]) == 0)) continue;
            n += snprintf(label + n, n < label_size ? label_size - n : 0, "%s%s", n ? ", " : "", parts[i]);
        }
    }
    cJSON_Delete(root);
    return ok;
}

const char *weather_describe(int code) {
    switch (code) {
        case 0: return "Clear";
        case 1: return "Mostly clear";
        case 2: return "Partly cloudy";
        case 3: return "Cloudy";
        case 45: case 48: return "Fog";
        case 51: case 53: case 55: case 56: case 57: return "Drizzle";
        case 61: return "Light rain";
        case 63: return "Rain";
        case 65: return "Heavy rain";
        case 66: case 67: return "Freezing rain";
        case 71: return "Light snow";
        case 73: return "Snow";
        case 75: return "Heavy snow";
        case 77: return "Snow grains";
        case 80: case 81: case 82: return "Showers";
        case 85: case 86: return "Snow showers";
        case 95: return "Thunderstorm";
        case 96: case 99: return "Storm, hail";
        default: return "";
    }
}

static double num_at(const cJSON *arr, int i, double fallback) {
    const cJSON *v = cJSON_GetArrayItem(arr, i);
    return cJSON_IsNumber(v) ? v->valuedouble : fallback;
}

bool weather_parse_forecast(const char *json, bool fahrenheit, weather_report_t *out) {
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_Parse(json ? json : "");
    const cJSON *cur = cJSON_GetObjectItemCaseSensitive(root, "current");
    const cJSON *temp = cJSON_GetObjectItemCaseSensitive(cur, "temperature_2m");
    const cJSON *daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    const cJSON *times = cJSON_GetObjectItemCaseSensitive(daily, "time");
    if (!cJSON_IsNumber(temp) || !cJSON_IsArray(times)) {
        cJSON_Delete(root);
        return false;
    }
    out->temperature = (float)temp->valuedouble;
    out->code = -1;
    const cJSON *code = cJSON_GetObjectItemCaseSensitive(cur, "weather_code");
    if (cJSON_IsNumber(code)) out->code = code->valueint;
    out->unit = fahrenheit ? 'F' : 'C';
    const cJSON *codes = cJSON_GetObjectItemCaseSensitive(daily, "weather_code");
    const cJSON *highs = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max");
    const cJSON *lows = cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min");
    const cJSON *rain = cJSON_GetObjectItemCaseSensitive(daily, "precipitation_probability_max");
    int n = cJSON_GetArraySize(times);
    for (int i = 0; i < n && out->day_count < WEATHER_MAX_DAYS; i++) {
        const cJSON *t = cJSON_GetArrayItem(times, i);
        weather_day_t *d = &out->days[out->day_count];
        if (!cJSON_IsString(t) || sscanf(t->valuestring, "%d-%d-%d", &d->y, &d->m, &d->d) != 3) continue;
        d->code = (int)num_at(codes, i, -1);
        d->high = (float)num_at(highs, i, 0);
        d->low = (float)num_at(lows, i, 0);
        d->precip_chance = (int)num_at(rain, i, -1);
        out->day_count++;
    }
    out->valid = true;
    cJSON_Delete(root);
    return true;
}
