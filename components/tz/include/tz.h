// Timezones without a timezone database: IANA names -> POSIX TZ strings (generated table, see
// tools/gen_tz.py), plus a small evaluator for those strings. The evaluator is pure and
// thread-safe, so iCal feeds in other timezones are converted without touching the process TZ.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;   // e.g. "America/Los_Angeles"
    const char *posix;  // e.g. "PST8PDT,M3.2.0,M11.1.0"
} tz_entry_t;

extern const tz_entry_t TZ_TABLE[];
extern const int TZ_TABLE_COUNT;

// POSIX string for an IANA name, or NULL if unknown.
const char *tz_posix_for(const char *name);

// IANA name for a Windows timezone name as Outlook/Exchange write it ("Pacific Standard Time"),
// or NULL if unknown.
const char *tz_windows_to_iana(const char *windows_name);

// A parsed POSIX TZ string. Offsets are seconds east of UTC (PST8 -> -28800).
typedef struct {
    char kind;     // 'M' (month.week.day), 'J' (Julian 1..365, no Feb 29) or 'D' (0..365)
    int m, w, d;   // 'M': month 1..12, week 1..5 (5 = last), weekday 0 = Sunday; 'J'/'D': day in d
    int time;      // seconds after local midnight (may be negative or above 24 h)
} tz_change_t;

typedef struct {
    int std_offset, dst_offset;
    bool has_dst;
    tz_change_t start, end;  // DST starts (in standard time) / ends (in DST)
} tz_rules_t;

bool tz_parse(const char *posix, tz_rules_t *out);  // false on a malformed string

int tz_rules_offset(const tz_rules_t *tz, int64_t utc);  // UTC offset in effect at instant utc
// Wall-clock time -> instant. A time skipped by a DST change is read with the offset before the
// change; a time that happens twice gives the first one (like Python's fold=0).
int64_t tz_rules_local_to_utc(const tz_rules_t *tz, int y, int mo, int d, int hh, int mi, int ss);

// Convenience forms on the string (UTC when it doesn't parse).
int tz_utc_offset(const char *posix, int64_t utc);
int64_t tz_local_to_utc(const char *posix, int y, int mo, int d, int hh, int mi, int ss);

#ifdef __cplusplus
}
#endif
