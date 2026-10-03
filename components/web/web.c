// Settings web server (see web.h). Secrets (Wi-Fi password, Google key) are accepted but never
// sent back. In run mode every API call except sign-in needs the session cookie, a request header
// that other sites can't send (X-HomePlanner), and the panel's own host name (no DNS rebinding).
#include "web.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gauth.h"
#include "gcal.h"
#include "ical.h"
#include "mdns.h"
#include "net.h"
#include "nvs.h"
#include "oauth.h"
#include "sdkconfig.h"
#include "tz.h"
#include "update.h"
#include "web_auth.h"

static const char *TAG = "web";
#define MAX_BODY (24 * 1024)
#define HOSTNAME "homeplanner"
#define UPCOMING_DAYS 30

extern const char page_start[] asm("_binary_page_html_start");
extern const char page_end[] asm("_binary_page_html_end");
void dns_start(void);

static web_hooks_t s_hooks;
static bool s_run_mode;
static char s_ip[16];
static web_auth_t s_auth;
static SemaphoreHandle_t s_lock;

#define LOCKED(stmt)                         \
    do {                                     \
        xSemaphoreTake(s_lock, portMAX_DELAY); \
        stmt;                                \
        xSemaphoreGive(s_lock);              \
    } while (0)

static void fill_random(void *buf, size_t len) { esp_fill_random(buf, len); }

// --- Sessions in NVS -----------------------------------------------------------------------------

static void sessions_load(void) {
    nvs_handle_t h;
    if (nvs_open("homeplanner", NVS_READONLY, &h) != ESP_OK) return;
    size_t len = sizeof s_auth.sessions;
    if (nvs_get_blob(h, "web_sessions", s_auth.sessions, &len) != ESP_OK || len != sizeof s_auth.sessions) {
        memset(s_auth.sessions, 0, sizeof s_auth.sessions);
    }
    nvs_close(h);
}

static void sessions_save(void) {  // with s_lock held
    nvs_handle_t h;
    if (nvs_open("homeplanner", NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, "web_sessions", s_auth.sessions, sizeof s_auth.sessions);
    nvs_commit(h);
    nvs_close(h);
}

void web_new_code(char code_out[7], time_t *expires) {
    LOCKED({
        snprintf(code_out, 7, "%s", web_auth_new_code(&s_auth, time(NULL), fill_random));
        *expires = (time_t)s_auth.code_expires;
    });
}

void web_cancel_code(void) { LOCKED(web_auth_cancel_code(&s_auth)); }

void web_sign_out_all(void) {
    LOCKED({
        web_auth_sign_out_all(&s_auth);
        sessions_save();
    });
}

int web_signed_in_count(void) {
    int n;
    LOCKED(n = web_auth_session_count(&s_auth, time(NULL)));
    return n;
}

// --- Helpers -------------------------------------------------------------------------------------

static esp_err_t send_json(httpd_req_t *req, cJSON *body) {
    char *text = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, text ? text : "{}");
    free(text);
    return err;
}

static esp_err_t send_error(httpd_req_t *req, const char *message) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", message);
    return send_json(req, o);
}

static esp_err_t send_ok(httpd_req_t *req) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    return send_json(req, o);
}

static bool header(httpd_req_t *req, const char *name, char *out, size_t size) {
    out[0] = '\0';
    return httpd_req_get_hdr_value_str(req, name, out, size) == ESP_OK;
}

// Run mode: only requests addressed to the panel by name or IP (a web page elsewhere can't point
// a look-alike name at it to read the answers).
static bool host_ok(httpd_req_t *req) {
    if (!s_run_mode) return true;
    char host[80];
    if (!header(req, "Host", host, sizeof host)) return false;
    char *colon = strchr(host, ':');
    if (colon) {
        if (strcmp(colon, ":80") != 0) return false;
        *colon = '\0';
    }
    return strcasecmp(host, HOSTNAME ".local") == 0 || strcmp(host, s_ip) == 0;
}

static bool session_token(httpd_req_t *req, char token[WEB_TOKEN_LEN + 1]) {
    size_t len = httpd_req_get_hdr_value_len(req, "Cookie");
    if (!len || len > 2048) return false;
    char *cookie = malloc(len + 1);
    bool ok = cookie && httpd_req_get_hdr_value_str(req, "Cookie", cookie, len + 1) == ESP_OK &&
              web_auth_cookie_token(cookie, token);
    free(cookie);
    return ok;
}

static bool signed_in(httpd_req_t *req) {
    if (!s_run_mode) return true;  // the hotspot is protected by its own password
    char token[WEB_TOKEN_LEN + 1];
    if (!session_token(req, token)) return false;
    bool ok;
    LOCKED(ok = web_auth_check(&s_auth, token, time(NULL)));
    return ok;
}

// Answers (and returns true) when a request isn't allowed. POSTs must come from our own page:
// other sites' forms can't send the X-HomePlanner header.
static bool refused(httpd_req_t *req, bool need_session) {
    if (!host_ok(req)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "Open the panel's page at http://" HOSTNAME ".local");
        return true;
    }
    char marker[8];
    if (req->method == HTTP_POST && (!header(req, "X-HomePlanner", marker, sizeof marker) || strcmp(marker, "1") != 0)) {
        httpd_resp_set_status(req, "403 Forbidden");
        httpd_resp_sendstr(req, "Forbidden");
        return true;
    }
    if (need_session && !signed_in(req)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        send_error(req, "Please sign in again: on the panel, tap the gear and choose Manage from computer.");
        return true;
    }
    return false;
}
#define GUARD(req, need_session) \
    do {                         \
        if (refused(req, need_session)) return ESP_OK; \
    } while (0)

static char *read_body(httpd_req_t *req, const char **problem) {
    *problem = NULL;
    if (req->content_len <= 0 || req->content_len > MAX_BODY) {
        *problem = "That's too large for the panel.";
        return NULL;
    }
    char *body = calloc(1, req->content_len + 1);
    if (!body) {
        *problem = "The panel is out of memory.";
        return NULL;
    }
    for (int got = 0; got < (int)req->content_len;) {
        int r = httpd_req_recv(req, body + got, req->content_len - got);
        if (r <= 0) {
            free(body);
            *problem = "The upload was interrupted. Please try again.";
            return NULL;
        }
        got += r;
    }
    return body;
}

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

// --- Pages ---------------------------------------------------------------------------------------

static esp_err_t page_get(httpd_req_t *req) {
    if (!host_ok(req)) {
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Location", "http://" HOSTNAME ".local/");
        return httpd_resp_send(req, NULL, 0);
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "X-Frame-Options", "DENY");
    httpd_resp_set_hdr(req, "Referrer-Policy", "no-referrer");
    return httpd_resp_send(req, page_start, page_end - page_start - 1);
}

// Setup mode: anything else (phones probing for internet) goes to the setup page.
static esp_err_t redirect_404(httpd_req_t *req, httpd_err_code_t code) {
    (void)code;
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
    return httpd_resp_send(req, NULL, 0);
}

static void add_calendars(cJSON *o) {
    hp_calendars_t *cals = calloc(1, sizeof *cals);  // ~11 KB: too big for the server's stack
    if (!cals) return;
    calendars_load(cals);
    cJSON *list = cJSON_AddArrayToObject(o, "calendars");
    for (int i = 0; i < cals->count; i++) {
        const hp_calendar_t *c = &cals->items[i];
        cJSON *j = cJSON_CreateObject();
        cJSON_AddStringToObject(j, "id", c->id);
        cJSON_AddStringToObject(j, "name", c->name);
        cJSON_AddStringToObject(j, "kind", c->google ? "google" : "ical");
        if (c->google) cJSON_AddStringToObject(j, "google_id", c->google_id);
        else cJSON_AddStringToObject(j, "url", c->url);
        cJSON_AddStringToObject(j, "color", c->color);
        web_calendar_status_t st = {0};
        if (s_hooks.calendar_status && s_hooks.calendar_status(c->id, &st)) {
            if (st.last_updated) cJSON_AddNumberToObject(j, "last_updated", (double)st.last_updated);
            if (st.error[0]) cJSON_AddStringToObject(j, "error", st.error);
        }
        cJSON_AddItemToArray(list, j);
    }
    free(cals);
    cJSON_AddNumberToObject(o, "max_calendars", HP_MAX_CALENDARS);
}

static esp_err_t state_get(httpd_req_t *req) {
    GUARD(req, false);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "mode", s_run_mode ? "run" : "setup");
    bool in = signed_in(req);
    cJSON_AddBoolToObject(o, "signed_in", in);
    if (!in) return send_json(req, o);

    hp_settings_t s;
    if (settings_load(&s)) {
        char *json = settings_to_json(&s);
        cJSON_AddItemToObject(o, "settings", cJSON_Parse(json));
        free(json);
    }
    cJSON *g = cJSON_AddObjectToObject(o, "google");
    cJSON_AddBoolToObject(g, "configured", gauth_configured());
    cJSON_AddBoolToObject(g, "connected", gauth_signed_in());
    cJSON_AddBoolToObject(g, "expired", gauth_revoked());
    cJSON_AddStringToObject(g, "email", gauth_email());
    cJSON *colors = cJSON_AddArrayToObject(o, "colors");  // family members: Google event colors
    for (int i = 1; i <= 11; i++) {
        char id[4];
        snprintf(id, sizeof id, "%d", i);
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "id", id);
        cJSON_AddStringToObject(c, "name", hp_color_name(id));
        cJSON_AddStringToObject(c, "hex", hp_color_hex(id));
        cJSON_AddItemToArray(colors, c);
    }
    cJSON *palette = cJSON_AddArrayToObject(o, "palette");  // other calendars: all 24
    for (int i = 0; i < HP_PALETTE_COUNT; i++) {
        cJSON *c = cJSON_CreateObject();
        cJSON_AddStringToObject(c, "id", HP_PALETTE[i].id);
        cJSON_AddStringToObject(c, "name", HP_PALETTE[i].name);
        cJSON_AddStringToObject(c, "hex", HP_PALETTE[i].hex);
        cJSON_AddItemToArray(palette, c);
    }
    if (s_run_mode) add_calendars(o);
    return send_json(req, o);
}

static esp_err_t timezones_get(httpd_req_t *req) {
    GUARD(req, true);
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < TZ_TABLE_COUNT; i++) cJSON_AddItemToArray(list, cJSON_CreateString(TZ_TABLE[i].name));
    return send_json(req, list);
}

static esp_err_t networks_get(httpd_req_t *req) {
    GUARD(req, true);
    net_ap_t aps[20];
    int n = net_scan(aps, 20);
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < n; i++) {
        cJSON *a = cJSON_CreateObject();
        cJSON_AddStringToObject(a, "ssid", aps[i].ssid);
        cJSON_AddNumberToObject(a, "rssi", aps[i].rssi);
        cJSON_AddBoolToObject(a, "secure", aps[i].secure);
        cJSON_AddItemToArray(list, a);
    }
    return send_json(req, list);
}

// --- Sign in -------------------------------------------------------------------------------------

static esp_err_t login_post(httpd_req_t *req) {
    GUARD(req, false);
    const char *problem;
    char *body = read_body(req, &problem);
    if (!body) return send_error(req, problem);
    cJSON *root = cJSON_Parse(body);
    free(body);
    char token[WEB_TOKEN_LEN + 1];
    web_login_t result;
    int left = 0;
    LOCKED({
        result = web_auth_login(&s_auth, jstr(root, "code"), time(NULL), fill_random, token);
        left = s_auth.tries_left;
        if (result == WEB_LOGIN_OK) sessions_save();
    });
    cJSON_Delete(root);
    if (result == WEB_LOGIN_NO_CODE) {
        return send_error(req, "There's no code on the panel right now. On the panel, tap the gear and choose Manage from computer.");
    }
    if (result == WEB_LOGIN_WRONG) {
        char msg[160];
        if (left > 0) snprintf(msg, sizeof msg, "That code isn't right (%d %s left).", left, left == 1 ? "try" : "tries");
        else snprintf(msg, sizeof msg, "That code isn't right. For safety the code was cancelled: show a new one on the panel.");
        return send_error(req, msg);
    }
    char cookie[160];
    snprintf(cookie, sizeof cookie, WEB_COOKIE "=%s; Max-Age=%d; Path=/; HttpOnly; SameSite=Strict", token, WEB_SESSION_SECONDS);
    httpd_resp_set_hdr(req, "Set-Cookie", cookie);
    ESP_LOGI(TAG, "a computer signed in");
    if (s_hooks.signed_in) s_hooks.signed_in();
    return send_ok(req);
}

static esp_err_t logout_post(httpd_req_t *req) {
    GUARD(req, false);
    char token[WEB_TOKEN_LEN + 1];
    if (session_token(req, token)) {
        LOCKED({
            web_auth_logout(&s_auth, token);
            sessions_save();
        });
    }
    httpd_resp_set_hdr(req, "Set-Cookie", WEB_COOKIE "=; Max-Age=0; Path=/; HttpOnly; SameSite=Strict");
    return send_ok(req);
}

// --- Settings ------------------------------------------------------------------------------------

static void restart_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));  // let the response reach the browser
    if (s_hooks.settings_saved) s_hooks.settings_saved();
    vTaskDelete(NULL);
}

static esp_err_t save_post(httpd_req_t *req) {
    GUARD(req, true);
    const char *problem;
    char *body = read_body(req, &problem);
    if (!body) return send_error(req, problem);

    hp_settings_t *s = calloc(2, sizeof *s);  // new + old (~2 KB together)
    if (!s) {
        free(body);
        return send_error(req, "The panel is out of memory.");
    }
    hp_settings_t *old = &s[1];
    bool had_old = settings_load(old);
    char err[160] = "";
    bool ok;
    if (!s_run_mode) {  // the hotspot page: Wi-Fi and timezone; everything else stays as it was
        hp_wifi_t w;
        ok = settings_wifi_from_json(body, &w, err, sizeof err);
        if (ok) {
            if (had_old) *s = *old;
            settings_apply_wifi(s, &w);
        }
    } else {
        bool has_pass;
        ok = settings_from_json(body, s, &has_pass, err, sizeof err);
        if (ok && !has_pass && had_old && strcmp(old->wifi_ssid, s->wifi_ssid) == 0) {
            memcpy(s->wifi_pass, old->wifi_pass, sizeof s->wifi_pass);  // "unchanged"
        }
        if (ok && had_old && s->place[0] && strcmp(old->place, s->place) == 0 && old->has_coords && !s->has_coords) {
            s->has_coords = true;  // same place: keep the coordinates already looked up
            s->latitude = old->latitude;
            s->longitude = old->longitude;
        }
    }
    free(body);
    if (ok && !settings_save(s)) {
        ok = false;
        snprintf(err, sizeof err, "Couldn't save the settings on the panel.");
    }
    if (!ok) {
        free(s);
        return send_error(req, err);
    }
    settings_request_setup(false);
    ESP_LOGI(TAG, "settings saved (network \"%s\", %s)", s->wifi_ssid, s->complete ? "complete" : "Wi-Fi only");

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "wifi_ssid", s->wifi_ssid);
    free(s);
    send_json(req, o);
    xTaskCreate(restart_task, "web_restart", 3072, NULL, 5, NULL);
    return ESP_OK;
}

// --- Sign in with Google ---------------------------------------------------------------------------

static struct {  // one sign-in at a time, single use, 10 minutes
    char verifier[OAUTH_VERIFIER_LEN + 1];
    char nonce[OAUTH_NONCE_LEN + 1];
    time_t expires;
} s_flow;

static esp_err_t google_start_post(httpd_req_t *req) {
    GUARD(req, true);
    if (!gauth_configured()) return send_error(req, "This panel's software has no Google sign-in set up (see the README).");
    // Google sends the browser back (via the relay page) to the address it used for this page.
    char host[80];
    header(req, "Host", host, sizeof host);
    char *colon = strchr(host, ':');
    if (colon) *colon = '\0';
    char challenge[OAUTH_CHALLENGE_LEN + 1], state[160];
    LOCKED({
        oauth_pkce(s_flow.verifier, challenge, fill_random);
        oauth_nonce(s_flow.nonce, fill_random);
        s_flow.expires = time(NULL) + 600;
    });
    if (!oauth_state_make(host, s_flow.nonce, state, sizeof state)) return send_error(req, "Open this page by the panel's address.");
    char *url = oauth_auth_url(CONFIG_HP_GOOGLE_CLIENT_ID, CONFIG_HP_GOOGLE_REDIRECT_URI, challenge, state);
    if (!url) return send_error(req, "The panel is out of memory.");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "url", url);
    free(url);
    return send_json(req, o);
}

static int hex_digit(char c) {
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static void url_decode(const char *in, char *out, size_t size) {
    size_t o = 0;
    for (const char *p = in; *p && o + 1 < size; p++) {
        if (*p == '%' && hex_digit(p[1]) >= 0 && hex_digit(p[2]) >= 0) {
            out[o++] = (char)(hex_digit(p[1]) * 16 + hex_digit(p[2]));
            p += 2;
        } else {
            out[o++] = *p == '+' ? ' ' : *p;
        }
    }
    out[o] = '\0';
}

static esp_err_t back_to_page(httpd_req_t *req, const char *query) {
    char location[256];
    snprintf(location, sizeof location, "/?%s", query);
    httpd_resp_set_status(req, "302 Found");
    httpd_resp_set_hdr(req, "Location", location);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, NULL, 0);
}

// Google -> relay page -> here. It arrives from another site, so without the session cookie: the
// single-use nonce in `state` (made for a signed-in browser) is what authorizes it.
static esp_err_t oauth_done_get(httpd_req_t *req) {
    if (!host_ok(req)) return back_to_page(req, "google=failed");
    size_t qlen = httpd_req_get_url_query_len(req);
    char *query = qlen && qlen < 4096 ? malloc(qlen + 1) : NULL;
    if (!query || httpd_req_get_url_query_str(req, query, qlen + 1) != ESP_OK) {
        free(query);
        return back_to_page(req, "google=failed");
    }
    char *code = calloc(1, 1024), state[160] = "", error[64] = "", host[64], nonce[OAUTH_NONCE_LEN + 1];
    if (!code) {
        free(query);
        return back_to_page(req, "google=failed");
    }
    httpd_query_key_value(query, "code", code, 1024);
    httpd_query_key_value(query, "state", state, sizeof state);
    httpd_query_key_value(query, "error", error, sizeof error);
    free(query);
    char decoded[1024];
    url_decode(code, decoded, sizeof decoded);  // the code is %-escaped (4%2F0A...)

    char verifier[OAUTH_VERIFIER_LEN + 1] = "";
    bool valid = oauth_state_parse(state, host, sizeof host, nonce, sizeof nonce);
    LOCKED({
        valid = valid && s_flow.expires > time(NULL) && strcmp(nonce, s_flow.nonce) == 0;
        if (valid) snprintf(verifier, sizeof verifier, "%s", s_flow.verifier);
        memset(&s_flow, 0, sizeof s_flow);  // single use, whatever happens
    });
    free(code);
    if (!valid) return back_to_page(req, "google=stale");
    if (error[0] || !decoded[0]) return back_to_page(req, "google=cancelled");

    char err[160];
    if (gauth_exchange_code(decoded, verifier, err, sizeof err) != ESP_OK) {
        ESP_LOGW(TAG, "Google sign-in failed: %s", err);
        return back_to_page(req, "google=failed");
    }
    if (s_hooks.google_changed) s_hooks.google_changed();
    return back_to_page(req, "google=connected");
}

static esp_err_t google_calendars_get(httpd_req_t *req) {
    GUARD(req, true);
    cJSON *items = NULL;
    char err[200];
    if (gcal_calendar_list(&items, err, sizeof err) != ESP_OK) return send_error(req, err);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddItemToObject(o, "calendars", items);
    return send_json(req, o);
}

static esp_err_t google_disconnect_post(httpd_req_t *req) {
    GUARD(req, true);
    gauth_sign_out();
    if (s_hooks.google_changed) s_hooks.google_changed();
    return send_ok(req);
}

// --- Other calendars -----------------------------------------------------------------------------

static bool count_one(const ical_cal_t *cal, const ical_occurrence_t *occ, void *ctx) {
    (void)cal, (void)occ;
    (*(int *)ctx)++;
    return true;
}

// Save a new calendar and tell the model. ics (may be NULL) is handed over; upcoming < 0: unknown.
static esp_err_t add_calendar(httpd_req_t *req, hp_calendars_t *cals, hp_calendar_t *cal, char *ics, size_t len, int upcoming) {
    uint8_t raw[6];
    esp_fill_random(raw, sizeof raw);
    for (int i = 0; i < 6; i++) snprintf(cal->id + 2 * i, 3, "%02x", raw[i]);
    cals->items[cals->count++] = *cal;
    if (!calendars_save(cals)) {
        free(ics);
        return send_error(req, "Couldn't save the calendar on the panel.");
    }
    ESP_LOGI(TAG, "calendar %s added (%s)", cal->id, cal->google ? "Google" : "iCal");
    if (s_hooks.calendar_added) s_hooks.calendar_added(cal, ics, len);
    else free(ics);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "id", cal->id);
    if (upcoming >= 0) {
        cJSON_AddNumberToObject(o, "upcoming", upcoming);
        cJSON_AddNumberToObject(o, "upcoming_days", UPCOMING_DAYS);
    }
    return send_json(req, o);
}

static esp_err_t add_downloaded(httpd_req_t *req, hp_calendars_t *cals, hp_calendar_t *cal) {
    calendars_load(cals);
    if (cals->count >= HP_MAX_CALENDARS) return send_error(req, "You can add up to 10 calendars. Remove one first.");
    for (int i = 0; i < cals->count; i++) {
        bool same = cal->google ? cals->items[i].google && strcmp(cals->items[i].google_id, cal->google_id) == 0
                                : !cals->items[i].google && strcmp(cals->items[i].url, cal->url) == 0;
        if (same) return send_error(req, "That calendar has already been added.");
    }
    if (cal->google) return add_calendar(req, cals, cal, NULL, 0, -1);  // the panel fetches it from Google

    // Download and read it now, so a wrong link is reported right away.
    char *ics = NULL, err[200];
    size_t len = 0;
    if (net_download(cal->url, WEB_MAX_ICS_BYTES, &ics, &len, err, sizeof err) != ESP_OK) return send_error(req, err);
    ical_status_t st;
    ical_cal_t *parsed = ical_parse(ics, len, &st);
    if (!parsed) {
        free(ics);
        return send_error(req, st == ICAL_NO_MEMORY ? "That calendar is too large for the panel."
                                                    : "That link didn't return a calendar (.ics / iCal).");
    }
    int upcoming = 0;
    const char *posix = getenv("TZ");
    ical_expand(parsed, posix ? posix : "UTC0", hp_local_date(time(NULL)), UPCOMING_DAYS, count_one, &upcoming);
    int events = ical_event_count(parsed);
    ical_free(parsed);

    ESP_LOGI(TAG, "calendar feed read (%d events, %d upcoming, %u bytes)", events, upcoming, (unsigned)len);
    return add_calendar(req, cals, cal, ics, len, upcoming);
}

static esp_err_t calendar_add_post(httpd_req_t *req) {
    GUARD(req, true);
    const char *problem;
    char *body = read_body(req, &problem);
    if (!body) return send_error(req, problem);
    cJSON *root = cJSON_Parse(body);
    free(body);
    hp_calendar_t cal = {0};
    const char *name = jstr(root, "name"), *color = jstr(root, "color"), *kind = jstr(root, "kind");
    cal.google = kind && strcmp(kind, "google") == 0;
    problem = calendar_check(name, color);
    if (!problem && cal.google) {
        problem = calendar_check_google_id(jstr(root, "google_id"));
        if (!problem) snprintf(cal.google_id, sizeof cal.google_id, "%s", jstr(root, "google_id"));
    } else if (!problem) {
        problem = calendar_normalize_url(jstr(root, "url"), cal.url, sizeof cal.url);
    }
    if (!problem) {
        snprintf(cal.name, sizeof cal.name, "%s", name);
        snprintf(cal.color, sizeof cal.color, "%s", color);
    }
    cJSON_Delete(root);
    if (problem) return send_error(req, problem);

    // Heap, not stack: the list is ~11 KB.
    hp_calendars_t *cals = calloc(1, sizeof *cals);
    if (!cals) return send_error(req, "The panel is out of memory.");
    esp_err_t res = add_downloaded(req, cals, &cal);
    free(cals);
    return res;
}

static esp_err_t calendar_update_post(httpd_req_t *req) {
    GUARD(req, true);
    const char *problem;
    char *body = read_body(req, &problem);
    if (!body) return send_error(req, problem);
    cJSON *root = cJSON_Parse(body);
    free(body);
    hp_calendars_t *cals = calloc(1, sizeof *cals);  // ~11 KB: too big for the server's stack
    if (!cals) {
        cJSON_Delete(root);
        return send_error(req, "The panel is out of memory.");
    }
    calendars_load(cals);
    hp_calendar_t *c = (hp_calendar_t *)calendars_find(cals, jstr(root, "id"));
    bool remove = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(root, "remove"));
    if (!c) problem = "That calendar was already removed.";
    else if (!remove) {
        const char *name = jstr(root, "name") ? jstr(root, "name") : c->name;
        const char *color = jstr(root, "color") ? jstr(root, "color") : c->color;
        problem = calendar_check(name, color);
        if (!problem) {
            char tmp[HP_NAME_LEN];
            snprintf(tmp, sizeof tmp, "%s", name);  // name may point into c
            snprintf(c->name, sizeof c->name, "%s", tmp);
            snprintf(c->color, sizeof c->color, "%s", color);
        }
    } else {
        int i = (int)(c - cals->items);
        memmove(&cals->items[i], &cals->items[i + 1], sizeof cals->items[0] * (size_t)(cals->count - i - 1));
        cals->count--;
    }
    cJSON_Delete(root);
    if (!problem && !calendars_save(cals)) problem = "Couldn't save the change on the panel.";
    free(cals);
    if (problem) return send_error(req, problem);
    if (s_hooks.calendars_changed) s_hooks.calendars_changed();
    return send_ok(req);
}

// --- Software update -----------------------------------------------------------------------------

static const char *STATE_NAMES[] = {"idle", "checking", "up_to_date", "available", "installing", "failed"};

static esp_err_t update_get(httpd_req_t *req) {
    GUARD(req, true);
    update_status_t st = update_status();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "current", st.current);
    cJSON_AddStringToObject(o, "state", STATE_NAMES[st.state]);
    cJSON_AddNumberToObject(o, "percent", st.percent);
    cJSON_AddStringToObject(o, "message", st.message);
    if (st.state == UPDATE_AVAILABLE) {
        cJSON_AddStringToObject(o, "latest", st.latest);
        cJSON_AddStringToObject(o, "notes", st.notes);
    }
    if (st.checked) cJSON_AddNumberToObject(o, "checked", (double)st.checked);
    return send_json(req, o);
}

static esp_err_t update_check_post(httpd_req_t *req) {
    GUARD(req, true);
    update_check_now();
    return send_ok(req);
}

static esp_err_t update_install_post(httpd_req_t *req) {
    GUARD(req, true);
    char err[160];
    if (update_install_available(err, sizeof err) != ESP_OK) return send_error(req, err);
    return send_ok(req);
}

// The firmware file as the raw request body (application/octet-stream), written to flash as it arrives.
static esp_err_t update_upload_post(httpd_req_t *req) {
    GUARD(req, true);
    char err[160];
    if (req->content_len <= 0) return send_error(req, "Choose a firmware file (homeplanner.bin).");
    if (update_upload_begin(req->content_len, err, sizeof err) != ESP_OK) return send_error(req, err);
    char *buf = malloc(4096);
    esp_err_t e = buf ? ESP_OK : ESP_ERR_NO_MEM;
    if (!buf) snprintf(err, sizeof err, "The panel is out of memory.");
    size_t got = 0;
    while (e == ESP_OK && got < req->content_len) {
        size_t want = req->content_len - got;
        // The first piece must hold the firmware's description (checked in update_upload_write).
        int r = httpd_req_recv(req, buf, want < 4096 ? want : 4096);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (r <= 0) {
            e = ESP_FAIL;
            snprintf(err, sizeof err, "The upload was interrupted. Please try again.");
            break;
        }
        if (got == 0) {  // fill the first piece completely before checking it
            while (r < 4096 && (size_t)r < want) {
                int more = httpd_req_recv(req, buf + r, (want < 4096 ? want : 4096) - r);
                if (more == HTTPD_SOCK_ERR_TIMEOUT) continue;
                if (more <= 0) break;
                r += more;
            }
        }
        e = update_upload_write(buf, (size_t)r, err, sizeof err);
        got += (size_t)r;
    }
    free(buf);
    if (e != ESP_OK) {
        update_upload_end(false, NULL, 0);
        return send_error(req, err);
    }
    if (update_upload_end(true, err, sizeof err) != ESP_OK) return send_error(req, err);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "message", "Installed. The panel is restarting into the new version.");
    return send_json(req, o);
}

// --- Start ---------------------------------------------------------------------------------------

static esp_err_t start(bool run_mode, const web_hooks_t *hooks) {
    if (hooks) s_hooks = *hooks;
    s_run_mode = run_mode;
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 16 * 1024;  // HTTPS downloads and iCal parsing run here
    cfg.max_uri_handlers = 20;
    cfg.lru_purge_enable = true;
    cfg.recv_wait_timeout = 15;
    cfg.send_wait_timeout = 15;
    httpd_handle_t server = NULL;
    esp_err_t err = httpd_start(&server, &cfg);
    if (err != ESP_OK) return err;
    const httpd_uri_t routes[] = {
        {.uri = "/", .method = HTTP_GET, .handler = page_get},
        {.uri = "/api/state", .method = HTTP_GET, .handler = state_get},
        {.uri = "/api/timezones", .method = HTTP_GET, .handler = timezones_get},
        {.uri = "/api/networks", .method = HTTP_GET, .handler = networks_get},
        {.uri = "/api/save", .method = HTTP_POST, .handler = save_post},
        {.uri = "/api/login", .method = HTTP_POST, .handler = login_post},
        {.uri = "/api/logout", .method = HTTP_POST, .handler = logout_post},
        {.uri = "/api/calendars/add", .method = HTTP_POST, .handler = calendar_add_post},
        {.uri = "/api/calendars/update", .method = HTTP_POST, .handler = calendar_update_post},
        {.uri = "/api/google/start", .method = HTTP_POST, .handler = google_start_post},
        {.uri = "/api/google/calendars", .method = HTTP_GET, .handler = google_calendars_get},
        {.uri = "/api/google/disconnect", .method = HTTP_POST, .handler = google_disconnect_post},
        {.uri = "/oauth/done", .method = HTTP_GET, .handler = oauth_done_get},
        {.uri = "/api/update", .method = HTTP_GET, .handler = update_get},
        {.uri = "/api/update/check", .method = HTTP_POST, .handler = update_check_post},
        {.uri = "/api/update/install", .method = HTTP_POST, .handler = update_install_post},
        {.uri = "/api/update/upload", .method = HTTP_POST, .handler = update_upload_post},
    };
    for (size_t i = 0; i < sizeof routes / sizeof routes[0]; i++) httpd_register_uri_handler(server, &routes[i]);
    if (!run_mode) httpd_register_err_handler(server, HTTPD_404_NOT_FOUND, redirect_404);
    return ESP_OK;
}

esp_err_t web_start_setup(const web_hooks_t *hooks) {
    dns_start();
    esp_err_t err = start(false, hooks);
    if (err == ESP_OK) ESP_LOGI(TAG, "setup page on http://192.168.4.1/");
    return err;
}

esp_err_t web_start_run(const web_hooks_t *hooks, const char *ip) {
    snprintf(s_ip, sizeof s_ip, "%s", ip ? ip : "");
    if (!s_lock) s_lock = xSemaphoreCreateMutex();
    LOCKED(sessions_load());
    esp_err_t err = start(true, hooks);
    if (err != ESP_OK) return err;
    if (mdns_init() == ESP_OK) {
        mdns_hostname_set(HOSTNAME);
        mdns_instance_name_set("HomePlanner");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "settings page on http://" HOSTNAME ".local/ and http://%s/", s_ip);
    } else {
        ESP_LOGW(TAG, "mDNS didn't start; settings page on http://%s/", s_ip);
    }
    return ESP_OK;
}
