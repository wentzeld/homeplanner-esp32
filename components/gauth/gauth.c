// Google access tokens from the stored refresh token, cached until ~5 minutes before expiry.
#include "gauth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "net.h"
#include "oauth.h"
#include "sdkconfig.h"
#include "settings.h"

static const char *TAG = "gauth";

static char s_refresh[512], s_email[128];
static char s_token[2048];
static time_t s_expires_at;
static bool s_revoked;
static SemaphoreHandle_t s_lock;

static SemaphoreHandle_t lock(void) {
    static StaticSemaphore_t storage;
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&storage);
    return s_lock;
}

bool gauth_configured(void) { return CONFIG_HP_GOOGLE_CLIENT_ID[0] && CONFIG_HP_GOOGLE_CLIENT_SECRET[0]; }

void gauth_init(const char *refresh_token, const char *email) {
    xSemaphoreTake(lock(), portMAX_DELAY);
    snprintf(s_refresh, sizeof s_refresh, "%s", refresh_token ? refresh_token : "");
    snprintf(s_email, sizeof s_email, "%s", email ? email : "");
    s_expires_at = 0;
    s_revoked = false;
    xSemaphoreGive(lock());
}

bool gauth_signed_in(void) { return s_refresh[0] && !s_revoked; }
const char *gauth_email(void) { return s_email; }
bool gauth_revoked(void) { return s_revoked; }

// POST to Google's token endpoint. The answer goes into a heap buffer (it's ~2.5 KB).
static esp_err_t token_request(const char *body, oauth_tokens_t *t, oauth_result_t *result, char *err, size_t err_size) {
    char *resp = NULL;
    int status = 0;
    esp_err_t e = net_https("POST", OAUTH_TOKEN_ENDPOINT, NULL, "application/x-www-form-urlencoded", body, &resp, &status);
    if (e != ESP_OK) {
        free(resp);
        snprintf(err, err_size, "Can't reach Google.");
        return ESP_ERR_TIMEOUT;
    }
    *result = oauth_parse_token(resp, t);
    if (*result != OAUTH_OK) ESP_LOGW(TAG, "token request failed (HTTP %d): %s", status, t->error);
    free(resp);
    return ESP_OK;
}

esp_err_t gauth_token(char *token, size_t size, char *err, size_t err_size) {
    xSemaphoreTake(lock(), portMAX_DELAY);
    esp_err_t result = ESP_OK;
    time_t now = time(NULL);
    if (!s_refresh[0] || s_revoked) {
        snprintf(err, err_size, s_revoked ? "The Google sign-in has expired. Sign in again on the settings page."
                                          : "Not signed in to Google yet.");
        result = ESP_ERR_INVALID_STATE;
    } else if (now + 300 >= s_expires_at) {
        char *body = oauth_refresh_body(CONFIG_HP_GOOGLE_CLIENT_ID, CONFIG_HP_GOOGLE_CLIENT_SECRET, s_refresh);
        oauth_tokens_t *t = heap_caps_calloc(1, sizeof *t, MALLOC_CAP_SPIRAM);
        oauth_result_t r = OAUTH_BAD;
        if (!body || !t) {
            snprintf(err, err_size, "Out of memory.");
            result = ESP_ERR_NO_MEM;
        } else if ((result = token_request(body, t, &r, err, err_size)) == ESP_OK) {
            if (r == OAUTH_OK) {
                snprintf(s_token, sizeof s_token, "%s", t->access_token);
                s_expires_at = now + t->expires_in;
            } else if (r == OAUTH_REVOKED) {
                s_revoked = true;
                snprintf(err, err_size, "The Google sign-in has expired. Sign in again on the settings page.");
                result = ESP_ERR_INVALID_STATE;
            } else {
                snprintf(err, err_size, "Google sign-in failed (%s).", t->error[0] ? t->error : "unreadable answer");
                result = ESP_FAIL;
            }
        }
        free(body);
        free(t);
    }
    if (result == ESP_OK) snprintf(token, size, "%s", s_token);
    xSemaphoreGive(lock());
    return result;
}

esp_err_t gauth_exchange_code(const char *code, const char *verifier, char *err, size_t err_size) {
    if (!gauth_configured()) {
        snprintf(err, err_size, "This panel's software has no Google sign-in set up.");
        return ESP_ERR_INVALID_STATE;
    }
    char *body = oauth_code_body(CONFIG_HP_GOOGLE_CLIENT_ID, CONFIG_HP_GOOGLE_CLIENT_SECRET, CONFIG_HP_GOOGLE_REDIRECT_URI,
                                 code, verifier);
    oauth_tokens_t *t = heap_caps_calloc(1, sizeof *t, MALLOC_CAP_SPIRAM);
    oauth_result_t r = OAUTH_BAD;
    esp_err_t result = ESP_ERR_NO_MEM;
    if (body && t) result = token_request(body, t, &r, err, err_size);
    if (result == ESP_OK && r != OAUTH_OK) {
        snprintf(err, err_size, "Google didn't accept the sign-in (%s). Please try again.", t->error[0] ? t->error : "unknown");
        result = ESP_FAIL;
    } else if (result == ESP_OK && !t->refresh_token[0]) {
        snprintf(err, err_size, "Google didn't give the panel lasting access. Please try again.");
        result = ESP_FAIL;
    } else if (result == ESP_OK && !settings_save_google(t->refresh_token, t->email)) {
        snprintf(err, err_size, "Couldn't save the sign-in on the panel.");
        result = ESP_FAIL;
    } else if (result == ESP_OK) {
        xSemaphoreTake(lock(), portMAX_DELAY);
        snprintf(s_refresh, sizeof s_refresh, "%s", t->refresh_token);
        snprintf(s_email, sizeof s_email, "%s", t->email);
        snprintf(s_token, sizeof s_token, "%s", t->access_token);
        s_expires_at = time(NULL) + t->expires_in;
        s_revoked = false;
        xSemaphoreGive(lock());
        settings_erase_old_key();  // the service-account key of older versions isn't needed anymore
        ESP_LOGI(TAG, "signed in to Google as %s", s_email);
    }
    if (result == ESP_ERR_NO_MEM) snprintf(err, err_size, "Out of memory.");
    free(body);
    free(t);
    return result;
}

void gauth_sign_out(void) {
    char refresh[sizeof s_refresh];
    xSemaphoreTake(lock(), portMAX_DELAY);
    snprintf(refresh, sizeof refresh, "%s", s_refresh);
    s_refresh[0] = s_email[0] = s_token[0] = '\0';
    s_expires_at = 0;
    s_revoked = false;
    xSemaphoreGive(lock());
    settings_erase_google();
    if (!refresh[0]) return;
    size_t size = 3 * strlen(refresh) + 8;
    char *body = malloc(size);
    if (!body) return;
    snprintf(body, size, "token=");
    oauth_url_encode(refresh, body + 6, size - 6);
    char *resp = NULL;
    int status = 0;
    net_https("POST", OAUTH_REVOKE_ENDPOINT, NULL, "application/x-www-form-urlencoded", body, &resp, &status);
    ESP_LOGI(TAG, "signed out of Google (revoke: HTTP %d)", status);
    free(resp);
    free(body);
}
