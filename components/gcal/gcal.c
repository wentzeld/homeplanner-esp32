#include "gcal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "gauth.h"
#include "net.h"

static const char *TAG = "gcal";
#define API "https://www.googleapis.com/calendar/v3/calendars/"
// Only the fields the display and the edit logic use (keeps responses small).
#define FIELDS                                                                                          \
    "items(id,status,summary,start,end,location,description,colorId,recurringEventId,originalStartTime," \
    "recurrence,organizer(email,self)),nextPageToken"

static void rfc3339_utc(int64_t t, char out[24]) {
    time_t tt = (time_t)t;
    struct tm tm;
    gmtime_r(&tt, &tm);
    strftime(out, 24, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static esp_err_t explain(int status, const char *calendar_id, char *err, size_t err_size) {
    (void)calendar_id;
    if (status == 404)
        snprintf(err, err_size, "Calendar not found. Choose the Family calendar again on the settings page.");
    else if (status == 403)
        snprintf(err, err_size, "Your Google account (%s) can't change this calendar. Choose another one on the settings page.",
                 gauth_email());
    else if (status == 401)
        snprintf(err, err_size, "Google sign-in problem. Sign in again on the settings page.");
    else
        snprintf(err, err_size, "Google Calendar error (HTTP %d).", status);
    return ESP_FAIL;
}

esp_err_t gcal_list(const char *calendar_id, int64_t time_min, int64_t time_max, cJSON **items, char *err,
                    size_t err_size) {
    *items = NULL;
    char token[2048];
    esp_err_t e = gauth_token(token, sizeof token, err, err_size);
    if (e != ESP_OK) return e;

    char id[256], tmin[24], tmax[24], fields[256], page[256] = "";
    net_url_encode(calendar_id, id, sizeof id);
    net_url_encode(FIELDS, fields, sizeof fields);
    rfc3339_utc(time_min, tmin);
    rfc3339_utc(time_max, tmax);
    cJSON *all = cJSON_CreateArray();
    for (int pages = 0; pages < 10; pages++) {
        char url[1024];
        snprintf(url, sizeof url,
                 API "%s/events?timeMin=%s&timeMax=%s&singleEvents=true&orderBy=startTime&maxResults=2500&fields=%s%s%s",
                 id, tmin, tmax, fields, page[0] ? "&pageToken=" : "", page);
        char *body = NULL;
        int status = 0;
        e = net_https("GET", url, token, NULL, NULL, &body, &status);
        if (e != ESP_OK) {
            snprintf(err, err_size, "Can't reach Google Calendar.");
            free(body);
            cJSON_Delete(all);
            return ESP_ERR_TIMEOUT;
        }
        if (status != 200) {
            ESP_LOGW(TAG, "list failed (HTTP %d): %.200s", status, body ? body : "");
            free(body);
            cJSON_Delete(all);
            return explain(status, calendar_id, err, err_size);
        }
        cJSON *root = cJSON_Parse(body);
        free(body);
        cJSON *list = cJSON_DetachItemFromObjectCaseSensitive(root, "items");
        while (list && list->child) cJSON_AddItemToArray(all, cJSON_DetachItemViaPointer(list, list->child));
        cJSON_Delete(list);
        const cJSON *next = cJSON_GetObjectItemCaseSensitive(root, "nextPageToken");
        if (cJSON_IsString(next)) net_url_encode(next->valuestring, page, sizeof page);
        else page[0] = '\0';
        cJSON_Delete(root);
        if (!page[0]) break;
    }
    ESP_LOGI(TAG, "%d events", cJSON_GetArraySize(all));
    *items = all;
    return ESP_OK;
}

// --- single-event calls for hp_edit -----------------------------------------------------------

static hp_edit_result_t request(const char *calendar_id, const char *method, const char *event_id, const cJSON *body,
                                cJSON **out) {
    char token[2048], err[160];
    if (gauth_token(token, sizeof token, err, sizeof err) != ESP_OK) return HP_EDIT_NETWORK;
    char cal[256], id[256] = "", url[640];
    net_url_encode(calendar_id, cal, sizeof cal);
    if (event_id) net_url_encode(event_id, id, sizeof id);
    snprintf(url, sizeof url, API "%s/events%s%s", cal, event_id ? "/" : "", id);
    char *text = body ? cJSON_PrintUnformatted(body) : NULL;
    char *resp = NULL;
    int status = 0;
    esp_err_t e = net_https(method, url, token, text ? "application/json" : NULL, text, &resp, &status);
    free(text);
    hp_edit_result_t r;
    if (e != ESP_OK) r = HP_EDIT_NETWORK;
    else if (status == 200 || status == 204) r = HP_EDIT_OK;
    else if (status == 404 || status == 410) r = HP_EDIT_NOT_FOUND;
    else if (status == 403) r = HP_EDIT_NOT_ALLOWED;
    else r = HP_EDIT_FAILED;
    if (r != HP_EDIT_OK) ESP_LOGW(TAG, "%s %s: HTTP %d %.200s", method, event_id ? event_id : "(new)", status, resp ? resp : "");
    if (r == HP_EDIT_OK && out) *out = cJSON_Parse(resp ? resp : "");
    free(resp);
    return r;
}

static hp_edit_result_t op_get(void *ctx, const char *id, cJSON **out) { return request(ctx, "GET", id, NULL, out); }
static hp_edit_result_t op_insert(void *ctx, const cJSON *body, cJSON **out) { return request(ctx, "POST", NULL, body, out); }
static hp_edit_result_t op_update(void *ctx, const char *id, const cJSON *body, cJSON **out) {
    return request(ctx, "PUT", id, body, out);
}
static hp_edit_result_t op_patch(void *ctx, const char *id, const cJSON *body, cJSON **out) {
    return request(ctx, "PATCH", id, body, out);
}
static hp_edit_result_t op_remove(void *ctx, const char *id) { return request(ctx, "DELETE", id, NULL, NULL); }

hp_calendar_ops_t gcal_ops(const char *calendar_id) {
    return (hp_calendar_ops_t){(void *)calendar_id, op_get, op_insert, op_update, op_patch, op_remove};
}

esp_err_t gcal_calendar_list(cJSON **items, char *err, size_t err_size) {
    *items = NULL;
    char *token = malloc(2048);
    if (!token) return ESP_ERR_NO_MEM;
    esp_err_t e = gauth_token(token, 2048, err, err_size);
    char *body = NULL;
    int status = 0;
    if (e == ESP_OK) {
        e = net_https("GET",
                      "https://www.googleapis.com/calendar/v3/users/me/calendarList?maxResults=250&minAccessRole=reader"
                      "&fields=items(id%2Csummary%2CsummaryOverride%2CbackgroundColor%2CaccessRole%2Cprimary)",
                      token, NULL, NULL, &body, &status);
        if (e != ESP_OK) {
            snprintf(err, err_size, "Can't reach Google Calendar.");
            e = ESP_ERR_TIMEOUT;
        } else if (status != 200) {
            e = explain(status, "", err, err_size);
        } else {
            cJSON *root = cJSON_Parse(body);
            cJSON *list = cJSON_DetachItemFromObjectCaseSensitive(root, "items");
            cJSON_Delete(root);
            *items = cJSON_IsArray(list) ? list : cJSON_CreateArray();
            if (!cJSON_IsArray(list)) cJSON_Delete(list);
        }
    }
    free(body);
    free(token);
    return e;
}
