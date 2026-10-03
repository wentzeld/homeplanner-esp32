// The panel's settings web page, in two modes:
//  - setup: on the panel's own hotspot (captive portal), no sign-in; Wi-Fi, Google and the rest.
//  - run: on the home network at http://homeplanner.local (and the panel's IP). A computer signs
//    in with a one-time code shown on the panel; then it manages other calendars (iCal links)
//    and all settings.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <time.h>

#include "esp_err.h"
#include "settings.h"

#define WEB_MAX_ICS_BYTES (8 * 1024 * 1024)

typedef struct {
    time_t last_updated;  // 0 = not yet
    char error[160];      // "" when the last refresh worked
} web_calendar_status_t;

// Called from the web server's task.
typedef struct {
    void (*settings_saved)(void);  // the app restarts
    // Run mode. calendar_added takes ownership of the downloaded feed (free() it).
    void (*calendar_added)(const hp_calendar_t *cal, char *ics, size_t len);
    void (*calendars_changed)(void);  // renamed, recolored or removed
    bool (*calendar_status)(const char *id, web_calendar_status_t *out);  // optional
    void (*signed_in)(void);          // a computer just signed in with the code (optional)
    void (*google_changed)(void);     // signed in to or out of Google (optional)
    // The screen as RGB565 pixels (malloc'd; the web server frees them), or NULL. Optional: when
    // set, GET /api/screenshot serves it as a BMP (demo builds, for README pictures).
    uint16_t *(*screenshot)(int *width, int *height);
} web_hooks_t;

esp_err_t web_start_setup(const web_hooks_t *hooks);
esp_err_t web_start_run(const web_hooks_t *hooks, const char *ip);

// For the panel's QR / "Manage from computer" screens. The code is written to code_out.
void web_new_code(char code_out[7], time_t *expires);
void web_cancel_code(void);
void web_sign_out_all(void);
int web_signed_in_count(void);
