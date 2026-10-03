// Settings JSON parsing and validation (pure C + cJSON; host-tested).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "settings.h"
#include "tz.h"

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

static bool parse_hhmm(const char *s, int *minutes) {
    int h, m;
    char extra;
    if (!s || sscanf(s, "%d:%d%c", &h, &m, &extra) != 2 || h < 0 || h > 23 || m < 0 || m > 59) return false;
    *minutes = h * 60 + m;
    return true;
}

#define FAIL(msg)                              \
    do {                                       \
        snprintf(err, err_size, "%s", msg);    \
        cJSON_Delete(root);                    \
        return false;                          \
    } while (0)

bool settings_from_json(const char *json, hp_settings_t *out, bool *has_wifi_pass, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_Parse(json);
    if (!cJSON_IsObject(root)) FAIL("The settings could not be read.");

    const char *ssid = jstr(root, "wifi_ssid");
    if (!ssid || !ssid[0]) FAIL("Choose your home Wi-Fi network.");
    if (strlen(ssid) > 32) FAIL("That Wi-Fi network name is too long.");
    snprintf(out->wifi_ssid, sizeof out->wifi_ssid, "%s", ssid);
    const char *pass = jstr(root, "wifi_pass");
    *has_wifi_pass = pass != NULL;
    if (pass) {
        if (strlen(pass) > 64) FAIL("That Wi-Fi password is too long.");
        snprintf(out->wifi_pass, sizeof out->wifi_pass, "%s", pass);
    }

    const char *cal = jstr(root, "calendar_id");
    if (!cal || !strchr(cal, '@')) FAIL("Enter the calendar ID (it contains an @, e.g. ...@group.calendar.google.com).");
    if (strlen(cal) >= sizeof out->cal.calendar_id) FAIL("That calendar ID is too long.");
    snprintf(out->cal.calendar_id, sizeof out->cal.calendar_id, "%s", cal);

    const char *tzname = jstr(root, "timezone");
    const char *posix = tz_posix_for(tzname);
    if (!posix) FAIL("Choose your timezone.");
    snprintf(out->cal.timezone, sizeof out->cal.timezone, "%s", tzname);
    snprintf(out->tz_posix, sizeof out->tz_posix, "%s", posix);

    const cJSON *members = cJSON_GetObjectItemCaseSensitive(root, "members");
    int n = cJSON_GetArraySize(members);
    if (!cJSON_IsArray(members) || n < 1) FAIL("Add at least one family member.");
    if (n > HP_MAX_MEMBERS) FAIL("Up to 8 family members are supported.");
    for (int i = 0; i < n; i++) {
        const cJSON *m = cJSON_GetArrayItem(members, i);
        const char *name = jstr(m, "name"), *color = jstr(m, "color_id");
        if (!name || !name[0]) FAIL("Every family member needs a name.");
        if (strlen(name) >= HP_NAME_LEN) FAIL("A family member's name is too long.");
        if (!hp_color_hex(color)) FAIL("Every family member needs a color.");
        for (int j = 0; j < i; j++) {
            if (strcmp(out->cal.members[j].name, name) == 0) FAIL("Family member names must be different.");
            if (strcmp(out->cal.members[j].color_id, color) == 0) FAIL("Each family member needs a different color.");
        }
        snprintf(out->cal.members[i].name, sizeof out->cal.members[i].name, "%s", name);
        snprintf(out->cal.members[i].color_id, sizeof out->cal.members[i].color_id, "%s", color);
    }
    out->cal.member_count = n;

    // Location for the weather: a city/ZIP the panel looks up once online, and/or coordinates.
    const char *place = jstr(root, "place");
    if (place) {
        while (*place == ' ') place++;
        snprintf(out->place, sizeof out->place, "%s", place);
        for (size_t n = strlen(out->place); n > 0 && out->place[n - 1] == ' '; n--) out->place[n - 1] = '\0';
    }
    const cJSON *lat = cJSON_GetObjectItemCaseSensitive(root, "latitude");
    const cJSON *lon = cJSON_GetObjectItemCaseSensitive(root, "longitude");
    if (cJSON_IsNumber(lat) && cJSON_IsNumber(lon)) {
        if (lat->valuedouble < -90 || lat->valuedouble > 90 || lon->valuedouble < -180 || lon->valuedouble > 180)
            FAIL("The latitude must be -90 to 90 and the longitude -180 to 180.");
        out->latitude = lat->valuedouble;
        out->longitude = lon->valuedouble;
        out->has_coords = true;
    } else if (!out->place[0]) {
        FAIL("Enter your city or ZIP code (for the weather).");
    }
    const char *units = jstr(root, "units");
    out->fahrenheit = units && strcmp(units, "fahrenheit") == 0;

    if (!parse_hhmm(jstr(root, "sleep_off"), &out->sleep_off_min) || !parse_hhmm(jstr(root, "sleep_on"), &out->sleep_on_min))
        FAIL("Enter the screen sleep times as HH:MM.");
    const cJSON *wake = cJSON_GetObjectItemCaseSensitive(root, "wake_minutes");
    out->wake_minutes = cJSON_IsNumber(wake) ? wake->valueint : 5;
    if (out->wake_minutes < 1 || out->wake_minutes > 60) FAIL("Wake time must be 1 to 60 minutes.");

    out->complete = true;
    cJSON_Delete(root);
    return true;
}

bool settings_wifi_from_json(const char *json, hp_wifi_t *out, char *err, size_t err_size) {
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_Parse(json ? json : "");
    if (!cJSON_IsObject(root)) FAIL("The settings could not be read.");
    const char *ssid = jstr(root, "wifi_ssid");
    if (!ssid || !ssid[0]) FAIL("Choose your home Wi-Fi network.");
    if (strlen(ssid) > 32) FAIL("That Wi-Fi network name is too long.");
    snprintf(out->ssid, sizeof out->ssid, "%s", ssid);
    const char *pass = jstr(root, "wifi_pass");
    out->has_pass = pass != NULL;
    if (pass) {
        if (strlen(pass) > 64) FAIL("That Wi-Fi password is too long.");
        snprintf(out->pass, sizeof out->pass, "%s", pass);
    }
    const char *tzname = jstr(root, "timezone");
    const char *posix = tz_posix_for(tzname);
    if (!posix) FAIL("Choose your timezone.");
    snprintf(out->timezone, sizeof out->timezone, "%s", tzname);
    snprintf(out->tz_posix, sizeof out->tz_posix, "%s", posix);
    cJSON_Delete(root);
    return true;
}

void settings_apply_wifi(hp_settings_t *s, const hp_wifi_t *w) {
    bool same_network = strcmp(s->wifi_ssid, w->ssid) == 0;
    snprintf(s->wifi_ssid, sizeof s->wifi_ssid, "%s", w->ssid);
    if (w->has_pass) snprintf(s->wifi_pass, sizeof s->wifi_pass, "%s", w->pass);
    else if (!same_network) s->wifi_pass[0] = '\0';
    snprintf(s->cal.timezone, sizeof s->cal.timezone, "%s", w->timezone);
    snprintf(s->tz_posix, sizeof s->tz_posix, "%s", w->tz_posix);
}

bool settings_from_stored_json(const char *json, hp_settings_t *out) {
    cJSON *root = cJSON_Parse(json ? json : "");
    bool partial = cJSON_IsFalse(cJSON_GetObjectItemCaseSensitive(root, "complete"));
    cJSON_Delete(root);
    char err[160];
    bool has_pass;
    if (!partial) return settings_from_json(json, out, &has_pass, err, sizeof err);
    hp_wifi_t w;
    memset(out, 0, sizeof *out);
    if (!settings_wifi_from_json(json, &w, err, sizeof err)) return false;
    settings_apply_wifi(out, &w);
    out->wake_minutes = 5;  // defaults the settings page starts from
    out->sleep_off_min = 22 * 60;
    out->sleep_on_min = 6 * 60 + 30;
    out->fahrenheit = strncmp(w.timezone, "America/", 8) == 0;
    return true;
}

char *settings_to_json(const hp_settings_t *s) {
    cJSON *o = cJSON_CreateObject();
    char buf[24];  // "HH:MM" (sized for any int so the compiler can prove no truncation)
    cJSON_AddStringToObject(o, "wifi_ssid", s->wifi_ssid);
    cJSON_AddStringToObject(o, "calendar_id", s->cal.calendar_id);
    cJSON_AddStringToObject(o, "timezone", s->cal.timezone);
    cJSON *members = cJSON_AddArrayToObject(o, "members");
    for (int i = 0; i < s->cal.member_count; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "name", s->cal.members[i].name);
        cJSON_AddStringToObject(m, "color_id", s->cal.members[i].color_id);
        cJSON_AddItemToArray(members, m);
    }
    if (s->place[0]) cJSON_AddStringToObject(o, "place", s->place);
    if (s->has_coords) {
        cJSON_AddNumberToObject(o, "latitude", s->latitude);
        cJSON_AddNumberToObject(o, "longitude", s->longitude);
    }
    cJSON_AddStringToObject(o, "units", s->fahrenheit ? "fahrenheit" : "celsius");
    snprintf(buf, sizeof buf, "%02d:%02d", s->sleep_off_min / 60, s->sleep_off_min % 60);
    cJSON_AddStringToObject(o, "sleep_off", buf);
    snprintf(buf, sizeof buf, "%02d:%02d", s->sleep_on_min / 60, s->sleep_on_min % 60);
    cJSON_AddStringToObject(o, "sleep_on", buf);
    cJSON_AddNumberToObject(o, "wake_minutes", s->wake_minutes);
    cJSON_AddBoolToObject(o, "complete", s->complete);
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    return text;
}

// --- Other calendars ----------------------------------------------------------------------------

const char *calendar_normalize_url(const char *url, char *out, size_t size) {
    static const char *BAD = "The link must start with https://, http:// or webcal://";
    if (!url) return BAD;
    while (*url == ' ' || *url == '\t' || *url == '\r' || *url == '\n') url++;
    size_t n = strlen(url);
    while (n && (url[n - 1] == ' ' || url[n - 1] == '\t' || url[n - 1] == '\r' || url[n - 1] == '\n')) n--;
    const char *colon = memchr(url, ':', n);
    if (!colon || colon[1] != '/' || colon[2] != '/') return BAD;
    size_t scheme_len = (size_t)(colon - url);
    char scheme[10] = "";
    if (scheme_len >= sizeof scheme) return BAD;
    for (size_t i = 0; i < scheme_len; i++) scheme[i] = (char)((url[i] >= 'A' && url[i] <= 'Z') ? url[i] + 32 : url[i]);
    scheme[scheme_len] = '\0';
    if (strcmp(scheme, "webcal") == 0 || strcmp(scheme, "webcals") == 0) strcpy(scheme, "https");
    if (strcmp(scheme, "http") != 0 && strcmp(scheme, "https") != 0) return BAD;
    const char *rest = colon + 3, *end = url + n;
    const char *hash = memchr(rest, '#', (size_t)(end - rest));
    if (hash) end = hash;
    size_t host_len = strcspn(rest, "/?#");
    if (host_len == 0 || rest + host_len > end) return BAD;
    for (const char *p = rest; p < end; p++) {
        if ((unsigned char)*p <= ' ') return "The link can't contain spaces.";
    }
    int len = snprintf(out, size, "%s://%.*s", scheme, (int)(end - rest), rest);
    if (len < 0 || (size_t)len >= size) return "That link is too long.";
    // Google's embed and sharing links show a web page, not the calendar data.
    if (host_len == strlen("calendar.google.com") && strncasecmp(rest, "calendar.google.com", host_len) == 0 &&
        !strstr(out, "/ical/")) {
        return "That's a Google Calendar web page link. Use the calendar's iCal link instead: in Google Calendar, "
               "Settings and sharing > Integrate calendar > Secret address in iCal format.";
    }
    return NULL;
}

const char *calendar_check_google_id(const char *id) {
    if (!id || !id[0] || strlen(id) >= HP_ID_LEN) return "Choose one of your Google calendars.";
    for (const char *p = id; *p; p++)
        if ((unsigned char)*p <= ' ') return "Choose one of your Google calendars.";
    return NULL;
}

const char *calendar_check(const char *name, const char *color) {
    if (!name || !name[0]) return "Give the calendar a name.";
    if (strlen(name) >= HP_NAME_LEN) return "That calendar name is too long.";
    if (!hp_palette_hex(color)) return "Choose a color for the calendar.";
    return NULL;
}

bool calendars_from_json(const char *json, hp_calendars_t *out) {
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_Parse(json ? json : "");
    if (!cJSON_IsArray(root)) {
        cJSON_Delete(root);
        return false;
    }
    const cJSON *item;
    cJSON_ArrayForEach(item, root) {
        if (out->count >= HP_MAX_CALENDARS) break;
        const char *id = jstr(item, "id"), *name = jstr(item, "name"), *url = jstr(item, "url"), *color = jstr(item, "color");
        const char *kind = jstr(item, "kind"), *gid = jstr(item, "google_id");
        hp_calendar_t *c = &out->items[out->count];
        memset(c, 0, sizeof *c);
        c->google = kind && strcmp(kind, "google") == 0;  // older lists have no kind: iCal
        if (!id || !id[0] || strlen(id) >= sizeof c->id || calendar_check(name, color)) continue;
        if (c->google ? calendar_check_google_id(gid) != NULL : calendar_normalize_url(url, c->url, sizeof c->url) != NULL) continue;
        if (c->google) snprintf(c->google_id, sizeof c->google_id, "%s", gid);
        snprintf(c->id, sizeof c->id, "%s", id);
        snprintf(c->name, sizeof c->name, "%s", name);
        snprintf(c->color, sizeof c->color, "%s", color);
        out->count++;
    }
    cJSON_Delete(root);
    return true;
}

char *calendars_to_json(const hp_calendars_t *c) {
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < c->count; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", c->items[i].id);
        cJSON_AddStringToObject(o, "name", c->items[i].name);
        cJSON_AddStringToObject(o, "kind", c->items[i].google ? "google" : "ical");
        if (c->items[i].google) cJSON_AddStringToObject(o, "google_id", c->items[i].google_id);
        else cJSON_AddStringToObject(o, "url", c->items[i].url);
        cJSON_AddStringToObject(o, "color", c->items[i].color);
        cJSON_AddItemToArray(list, o);
    }
    char *text = cJSON_PrintUnformatted(list);
    cJSON_Delete(list);
    return text;
}

const hp_calendar_t *calendars_find(const hp_calendars_t *c, const char *id) {
    for (int i = 0; id && i < c->count; i++)
        if (strcmp(c->items[i].id, id) == 0) return &c->items[i];
    return NULL;
}
