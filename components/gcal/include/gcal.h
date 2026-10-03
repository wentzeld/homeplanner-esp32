// Google Calendar API v3 calls (as the signed-in Google account).
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"
#include "hp_edit.h"

// All events (repeats expanded) overlapping [time_min, time_max) (Unix seconds), as a cJSON
// array of Google event objects (caller deletes). On error, err gets a user-facing reason and
// the return value tells network problems (ESP_ERR_TIMEOUT) from others.
esp_err_t gcal_list(const char *calendar_id, int64_t time_min, int64_t time_max, cJSON **items, char *err,
                    size_t err_size);

// Google Calendar operations for hp_edit (add / edit / delete) on this calendar.
// The returned struct refers to calendar_id, which must stay valid.
hp_calendar_ops_t gcal_ops(const char *calendar_id);

// The signed-in account's calendars (calendarList items: id, summary, summaryOverride,
// backgroundColor, accessRole, primary) as a cJSON array (caller deletes).
esp_err_t gcal_calendar_list(cJSON **items, char *err, size_t err_size);
