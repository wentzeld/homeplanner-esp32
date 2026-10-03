// Settings persistence in NVS (namespace "homeplanner"). The Google key is stored as a blob.
// Not encrypted (by design decision; see README "Security").
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "settings.h"

static const char *TAG = "settings";
#define NS "homeplanner"

static char *get_blob(nvs_handle_t h, const char *key) {
    size_t len = 0;
    if (nvs_get_blob(h, key, NULL, &len) != ESP_OK || len == 0) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    if (nvs_get_blob(h, key, buf, &len) != ESP_OK) {
        free(buf);
        return NULL;
    }
    buf[len] = '\0';
    return buf;
}

// Strings (may be empty, e.g. an open Wi-Fi network's password).
static char *get_str(nvs_handle_t h, const char *key) {
    size_t len = 0;
    if (nvs_get_str(h, key, NULL, &len) != ESP_OK) return NULL;
    char *buf = malloc(len);
    if (buf && nvs_get_str(h, key, buf, &len) != ESP_OK) {
        free(buf);
        return NULL;
    }
    return buf;
}

static bool put_str(const char *key, const char *value) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_str(h, key, value) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

static bool put_blob(const char *key, const char *value) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return false;
    bool ok = nvs_set_blob(h, key, value, strlen(value)) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool settings_load(hp_settings_t *out) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char *json = get_blob(h, "settings");
    char *pass = get_str(h, "wifi_pass");
    nvs_close(h);
    bool ok = false;
    if (json) {
        ok = settings_from_stored_json(json, out);
        if (!ok) ESP_LOGW(TAG, "stored settings invalid");
        if (ok && pass) snprintf(out->wifi_pass, sizeof out->wifi_pass, "%s", pass);
    }
    free(json);
    free(pass);
    return ok;
}

bool settings_save(const hp_settings_t *s) {
    char *json = settings_to_json(s);
    bool ok = json && put_blob("settings", json) && put_str("wifi_pass", s->wifi_pass);
    free(json);
    return ok;
}

bool settings_load_google(char *refresh_token, size_t token_size, char *email, size_t email_size) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    char *token = get_str(h, "g_refresh"), *mail = get_str(h, "g_email");
    nvs_close(h);
    bool ok = token && token[0];
    if (ok) {
        snprintf(refresh_token, token_size, "%s", token);
        snprintf(email, email_size, "%s", mail ? mail : "");
    }
    free(token);
    free(mail);
    return ok;
}

bool settings_save_google(const char *refresh_token, const char *email) {
    return put_str("g_refresh", refresh_token) && put_str("g_email", email ? email : "");
}

static void erase_keys(const char *a, const char *b) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_erase_key(h, a);
    if (b) nvs_erase_key(h, b);
    nvs_commit(h);
    nvs_close(h);
}

void settings_erase_google(void) { erase_keys("g_refresh", "g_email"); }
void settings_erase_old_key(void) { erase_keys("sa_key", NULL); }

bool settings_setup_requested(void) {
    nvs_handle_t h;
    uint8_t v = 0;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "setup", &v);
        nvs_close(h);
    }
    return v != 0;
}

void settings_request_setup(bool on) {
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_u8(h, "setup", on ? 1 : 0);
    nvs_commit(h);
    nvs_close(h);
}

bool calendars_load(hp_calendars_t *out) {
    memset(out, 0, sizeof *out);
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return true;
    char *json = get_blob(h, "calendars");
    nvs_close(h);
    bool ok = !json || calendars_from_json(json, out);
    if (!ok) ESP_LOGW(TAG, "stored calendar list unreadable");
    free(json);
    return ok;
}

bool calendars_save(const hp_calendars_t *c) {
    char *json = calendars_to_json(c);
    bool ok = json && put_blob("calendars", json);
    free(json);
    return ok;
}
