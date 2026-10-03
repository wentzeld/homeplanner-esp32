// iCal (.ics) feeds for "other calendars": parse once, then expand repeating events for the days
// on screen. Pure C (plus the tz and logic components), unit-tested on the host against the
// Raspberry Pi version (recurring_ical_events).
//
// Repeats: RRULE FREQ=DAILY/WEEKLY/MONTHLY/YEARLY with INTERVAL, COUNT, UNTIL, BYDAY (also 2TU,
// -1FR), BYMONTHDAY, BYMONTH, BYSETPOS, WKST; EXDATE, RDATE, RECURRENCE-ID overrides (highest
// SEQUENCE wins) and STATUS:CANCELLED. Other rules show only their first occurrence.
// Times: DATE (all-day), floating (panel time), UTC and TZID (IANA, Windows/Outlook names,
// vendor-prefixed names, or the feed's own VTIMEZONE rules).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "hp_logic.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct ical_cal ical_cal_t;

typedef enum { ICAL_OK, ICAL_NOT_CALENDAR, ICAL_NO_MEMORY } ical_status_t;

// Parse a whole feed. NULL (and *status set) when it isn't a VCALENDAR or memory runs out.
ical_cal_t *ical_parse(const char *text, size_t len, ical_status_t *status);
void ical_free(ical_cal_t *cal);

int ical_event_count(const ical_cal_t *cal);
int ical_unsupported_count(const ical_cal_t *cal);  // events whose repeat rule we can't expand

typedef struct {
    int event;            // index of the VEVENT the details come from
    int64_t start, end;   // UTC seconds; all-day: panel-local midnights, end exclusive
    bool all_day;
} ical_occurrence_t;

// Called for each occurrence; return false to stop early.
typedef bool (*ical_visit_t)(const ical_cal_t *cal, const ical_occurrence_t *occ, void *ctx);

// Every occurrence overlapping the panel-local days [first, first + days), where the panel's
// timezone is local_posix (a POSIX TZ string; floating and all-day times are read in it).
// Returns the number visited. Thread-safe for a calendar that isn't being freed.
int ical_expand(const ical_cal_t *cal, const char *local_posix, hp_date_t first, int days,
                ical_visit_t visit, void *ctx);

// Display event for an occurrence: read-only, labelled with the calendar's name and color.
void ical_occurrence_to_event(const ical_cal_t *cal, const ical_occurrence_t *occ, const char *cal_id,
                              const char *cal_name, const char *color, hp_event_t *out);

#ifdef __cplusplus
}
#endif
