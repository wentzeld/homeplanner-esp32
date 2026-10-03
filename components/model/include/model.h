// The panel's data: Family calendar events and weather, kept in sync in the background and
// cached on flash (LittleFS) so the calendar shows immediately after a reboot, even offline.
#pragma once

#include <stdbool.h>
#include <time.h>

#include "hp_edit.h"
#include "hp_logic.h"
#include "settings.h"
#include "weather.h"

typedef struct {
    time_t last_synced;     // 0 = never
    bool error;             // last calendar sync failed
    bool network_error;     // ...because the network/Google couldn't be reached
    char message[200];      // user-facing reason when error
    bool syncing;
} model_status_t;

typedef void (*model_listener_t)(void);

// Mount storage, load the cached copy, start the sync tasks. `on_change` is called from the sync
// task (not the UI task) whenever events, weather or status change.
void model_start(const hp_settings_t *settings, model_listener_t on_change);

// Read access: hold the lock while using the returned pointers.
void model_lock(void);
void model_unlock(void);
const hp_event_t *model_events(int *count);
const weather_report_t *model_weather(void);
model_status_t model_status(void);
const hp_config_t *model_config(void);

// Make sure events for the week starting `sunday` are loaded (fetches on demand if it's
// outside the normal window).
void model_want_week(hp_date_t sunday);

// Refresh calendar and weather now.
void model_refresh_now(void);

// --- Other calendars (iCal links) ------------------------------------------------------------------
// Their events are merged into model_events(): read-only, with `calendar` set to the calendar's
// name. Each is downloaded every 30 minutes (every 5 while failing) and the last good copy is kept
// on flash, so they show after a reboot without internet.

// A calendar the settings page just added and downloaded; takes ownership of ics (free()).
void model_calendar_added(const hp_calendar_t *cal, char *ics, size_t len);
// The list changed (renamed, recolored, removed): reload it from settings.
void model_calendars_changed(void);
// Last good download and the last error ("" if none). False if the calendar is unknown.
bool model_calendar_status(const char *id, time_t *last_updated, char *error, size_t error_size);
// A copy of the list (for the legend); returns the count. The version changes with every edit.
int model_calendars(hp_calendar_t *out, int max);
unsigned model_calendars_version(void);

// --- Add / edit / delete (run in the background; the callback runs in a worker task) ---------
typedef void (*model_edit_cb_t)(hp_edit_result_t result, const char *message, const hp_event_details_t *details);

void model_edit_details(const char *event_id, model_edit_cb_t cb);
void model_edit_add(const hp_form_t *form, model_edit_cb_t cb);
void model_edit_update(const char *event_id, const hp_form_t *form, bool all_events, model_edit_cb_t cb);
void model_edit_delete(const char *event_id, bool all_events, model_edit_cb_t cb);
