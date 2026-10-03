// HomePlanner calendar logic: pure C (plus cJSON), no ESP-IDF calls, so it is unit-tested on
// the host. A port of the Raspberry Pi version's events.py / sync.py rules.
//
// Time: instants are Unix seconds (UTC). "Local" means the process timezone (TZ env var, POSIX
// format), which the firmware sets from the configured timezone and the host tests set directly.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"

#ifdef __cplusplus
extern "C" {
#endif

#define HP_MAX_MEMBERS 8
#define HP_ID_LEN 160
#define HP_TITLE_LEN 200
#define HP_TEXT_LEN 1024
#define HP_NAME_LEN 48

// --- Colors -------------------------------------------------------------------------------

#define HP_FAMILY_COLOR "#8d99ae"  // events with no member color

const char *hp_color_hex(const char *color_id);   // "1".."11" -> "#rrggbb", NULL if unknown
const char *hp_color_name(const char *color_id);  // "4" -> "Flamingo", NULL if unknown

// Google's full calendar palette (24 colors, picker order) for other calendars: the event
// colors "1".."11" plus named ones like "cherry-blossom".
typedef struct {
    const char *id, *name, *hex;
} hp_palette_color_t;
#define HP_PALETTE_COUNT 24
extern const hp_palette_color_t HP_PALETTE[HP_PALETTE_COUNT];
const char *hp_palette_hex(const char *id);  // NULL if unknown

// --- Config subset the logic needs ---------------------------------------------------------

typedef struct {
    char name[HP_NAME_LEN];
    char color_id[4];  // Google event color "1".."11"
} hp_member_t;

typedef struct {
    char calendar_id[HP_ID_LEN];
    char timezone[64];  // IANA name, sent to Google with local times (e.g. "America/Los_Angeles")
    hp_member_t members[HP_MAX_MEMBERS];
    int member_count;
} hp_config_t;

const char *hp_member_for_color(const hp_config_t *cfg, const char *color_id);  // NULL if none
const char *hp_color_for_member(const hp_config_t *cfg, const char *member);    // NULL if unknown

// --- Dates ---------------------------------------------------------------------------------

typedef struct {
    int y, m, d;  // m 1..12
} hp_date_t;

int64_t hp_days_from_civil(hp_date_t d);  // days since 1970-01-01 (pure calendar arithmetic)
hp_date_t hp_civil_from_days(int64_t days);
hp_date_t hp_date_add(hp_date_t d, int days);
int hp_date_cmp(hp_date_t a, hp_date_t b);
int hp_date_diff(hp_date_t a, hp_date_t b);  // a - b in days
int hp_weekday(hp_date_t d);                 // 0 = Sunday
hp_date_t hp_week_start(hp_date_t d);        // the Sunday on or before d
bool hp_parse_date(const char *s, hp_date_t *out);            // "YYYY-MM-DD"
void hp_format_date(hp_date_t d, char out[11]);               // "YYYY-MM-DD"

int64_t hp_local_midnight(hp_date_t d);                       // local 00:00 of d, as UTC seconds
hp_date_t hp_local_date(int64_t t);                           // local calendar date of instant t
bool hp_parse_rfc3339(const char *s, int64_t *out);          // "2026-10-05T17:00:00-07:00" / "Z"

// --- Text ----------------------------------------------------------------------------------

// Google Calendar stores notes as HTML (from its website). Convert to plain text for display:
// tags removed, line breaks for <br>/<p>/<div>/<li>..., entities decoded, at most one blank line.
void hp_html_to_text(const char *html, char *out, size_t size);

// --- Events --------------------------------------------------------------------------------

typedef struct {
    char id[HP_ID_LEN];
    char title[HP_TITLE_LEN];
    int64_t start, end;  // all-day: local midnights, end exclusive
    bool all_day;
    char location[HP_TEXT_LEN / 4];
    char description[HP_TEXT_LEN];
    char member[HP_NAME_LEN];  // "" when untagged
    char color[8];             // "#rrggbb"
    bool editable, recurring, owned;
    char series_id[HP_ID_LEN];
    char calendar[HP_NAME_LEN];  // other (iCal) calendar's name; "" for the Family calendar
} hp_event_t;

// Google API event JSON -> display event. Returns false for cancelled events or bad data.
bool hp_event_from_google(const cJSON *ev, const hp_config_t *cfg, hp_event_t *out);
// True when the Family calendar organizes the event (it may change details). Missing organizer = owned.
bool hp_is_owned(const cJSON *ev, const hp_config_t *cfg);
// Does the event overlap local day d?
bool hp_overlaps_day(const hp_event_t *e, hp_date_t d);

// --- The add/edit form ------------------------------------------------------------------------

typedef enum { HP_REPEAT_NONE, HP_REPEAT_DAILY, HP_REPEAT_WEEKLY, HP_REPEAT_MONTHLY, HP_REPEAT_YEARLY } hp_repeat_t;

typedef struct {
    char title[HP_TITLE_LEN];
    hp_date_t date;
    bool all_day;
    int start_min, end_min;  // minutes after midnight (ignored when all_day)
    char member[HP_NAME_LEN];  // "" = whole family
    char location[HP_TEXT_LEN / 4];
    char description[HP_TEXT_LEN];
    hp_repeat_t repeat;
    bool has_until;
    hp_date_t until;
} hp_form_t;

// NULL if valid, else a user-facing message.
const char *hp_form_validate(const hp_form_t *f);
// Insert payload. NULL (and *err set) on unknown member. Caller frees with cJSON_Delete.
cJSON *hp_form_to_google(const hp_form_t *f, const hp_config_t *cfg, const char **err);

// Repeat rules the form can express. Returns true and fills repeat/until for a simple rule
// (or HP_REPEAT_NONE when there is no RRULE); false for a custom rule that must be kept as-is.
bool hp_parse_simple_rrule(const cJSON *recurrence, hp_date_t start, hp_repeat_t *repeat,
                           bool *has_until, hp_date_t *until);

int hp_all_day_span(const cJSON *ev);  // days covered by an all-day event (1 for timed)
hp_date_t hp_occurrence_date(const cJSON *ev);  // local date the series put this event on

typedef struct {
    hp_form_t form;
    bool recurring, custom_repeat, owned, form_editable;
    int span_days;
} hp_event_details_t;

// Pre-fill the edit form. series may be NULL (an occurrence's repeat comes from its series).
bool hp_event_to_form(const cJSON *ev, const cJSON *series, const hp_config_t *cfg, hp_event_details_t *out);

// The resource updated with the form: cleared member/location/notes removed; recurrence kept
// (keep_recurrence) or replaced while preserving EXDATE/RDATE lines. Caller frees.
cJSON *hp_apply_form(const cJSON *resource, const hp_form_t *f, const hp_config_t *cfg,
                     bool keep_recurrence, int span_days, const char **err);

// Fields (besides who/repeat) the form changes vs. the event's current form data; for guest
// events these can't be saved. Bit flags below; 0 = only who/repeat changed.
enum { HP_CHG_TITLE = 1, HP_CHG_DATE = 2, HP_CHG_TIME = 4, HP_CHG_LOCATION = 8, HP_CHG_NOTES = 16 };
int hp_guest_changes(const hp_form_t *current, const hp_form_t *f);

#ifdef __cplusplus
}
#endif
