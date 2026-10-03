// HomePlanner settings: what the setup page collects. JSON parsing/validation is pure C
// (host-tested); settings_store.c persists to NVS on the device.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "hp_logic.h"

typedef struct {
    char wifi_ssid[33];
    char wifi_pass[65];
    hp_config_t cal;     // calendar_id, IANA timezone, members
    char tz_posix[64];   // derived from cal.timezone
    char place[64];      // city or ZIP code as typed; "" when coordinates were entered directly
    bool has_coords;     // false until the panel has looked up `place` online
    double latitude, longitude;
    bool fahrenheit;
    int sleep_off_min, sleep_on_min;  // minutes after midnight
    int wake_minutes;
    bool complete;  // false after the hotspot page (Wi-Fi only): the rest is filled in on the run page
} hp_settings_t;

// What the hotspot page sends: just enough to get the panel online.
typedef struct {
    char ssid[33];
    char pass[65];
    bool has_pass;      // false: keep the stored password (same network)
    char timezone[64];  // IANA name
    char tz_posix[64];
} hp_wifi_t;

// Parse + validate the full settings JSON (as sent by the settings page). On error returns false and
// writes a user-facing message to err. Secrets: "wifi_pass" may be absent (caller keeps the old one).
// Sets out->complete.
bool settings_from_json(const char *json, hp_settings_t *out, bool *has_wifi_pass, char *err, size_t err_size);

// The hotspot page's Wi-Fi + timezone. Same messages as above.
bool settings_wifi_from_json(const char *json, hp_wifi_t *out, char *err, size_t err_size);
// Put Wi-Fi/timezone into settings (keeping the rest; a new panel stays incomplete).
void settings_apply_wifi(hp_settings_t *s, const hp_wifi_t *w);

// JSON for the settings page and storage (never includes the Wi-Fi password). Caller frees.
char *settings_to_json(const hp_settings_t *s);
// Stored JSON back: full settings, or (for "complete": false) just the Wi-Fi part.
bool settings_from_stored_json(const char *json, hp_settings_t *out);

// --- Other calendars (iCal links), shown read-only next to the Family calendar ------------------

#define HP_MAX_CALENDARS 10
#define HP_URL_LEN 1024

typedef struct {
    char id[13];             // random, 12 hex characters
    char name[HP_NAME_LEN];
    bool google;             // a calendar of the signed-in Google account (else an iCal link)
    char url[HP_URL_LEN];    // iCal: normalized, https:// for webcal://
    char google_id[HP_ID_LEN];  // Google: its calendar ID
    char color[16];          // an HP_PALETTE id
} hp_calendar_t;

typedef struct {
    hp_calendar_t items[HP_MAX_CALENDARS];
    int count;
} hp_calendars_t;

// webcal:// -> https://, fragment dropped; only http(s) links with a host. NULL when OK, else a
// user-facing message.
const char *calendar_normalize_url(const char *url, char *out, size_t size);
// A Google calendar ID as listed by Google. NULL when OK, else a user-facing message.
const char *calendar_check_google_id(const char *id);
// Name and color check (adding or editing). NULL when OK, else a user-facing message.
const char *calendar_check(const char *name, const char *color);
// The stored list (invalid entries are dropped). Returns false for unreadable JSON.
bool calendars_from_json(const char *json, hp_calendars_t *out);
char *calendars_to_json(const hp_calendars_t *c);  // caller frees
const hp_calendar_t *calendars_find(const hp_calendars_t *c, const char *id);

// --- device storage (settings_store.c) ---------------------------------------------------------
bool settings_load(hp_settings_t *out);                 // false if never set up (complete or not)
bool settings_save(const hp_settings_t *s);
// The Google sign-in: refresh token and the account's email. Load returns false if not signed in.
bool settings_load_google(char *refresh_token, size_t token_size, char *email, size_t email_size);
bool settings_save_google(const char *refresh_token, const char *email);
void settings_erase_google(void);
void settings_erase_old_key(void);                      // the service-account key of older versions
bool settings_setup_requested(void);                    // "Change settings" was chosen
void settings_request_setup(bool on);
bool calendars_load(hp_calendars_t *out);               // empty list if none
bool calendars_save(const hp_calendars_t *c);
