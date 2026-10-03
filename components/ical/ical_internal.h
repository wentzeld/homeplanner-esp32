// Shared between the parser and the expander.
#pragma once

#include <stdint.h>

#include "ical.h"
#include "tz.h"

enum { T_NONE, T_DATE, T_FLOAT, T_UTC, T_ZONED };

typedef struct {
    int16_t y;
    uint8_t mo, d, h, mi, s;
    uint8_t kind;  // T_*
    int16_t zone;  // index into cal->zones when T_ZONED
} itime_t;

typedef struct {
    uint32_t uid, summary, location, description, rrule;  // offsets into the string pool (0 = "")
    itime_t start, end, rid;
    int32_t duration;  // DURATION in seconds (has_duration)
    int32_t sequence;
    uint32_t exdates, n_exdates, rdates, n_rdates;  // ranges in cal->times
    bool has_duration, cancelled;
} ievent_t;

typedef struct {
    uint32_t name;  // pool offset
    tz_rules_t rules;
    bool known;     // false: read like a floating time (panel timezone)
} izone_t;

struct ical_cal {
    ievent_t *events;
    int n_events, cap_events;
    itime_t *times;
    uint32_t n_times, cap_times;
    izone_t *zones;
    int n_zones, cap_zones;
    char *pool;
    size_t pool_len, pool_cap;
    int *by_uid;     // event indexes sorted by UID
    int unsupported;
};

void *ical_realloc(void *p, size_t size);
static inline const char *ical_str(const ical_cal_t *cal, uint32_t off) { return cal->pool + off; }

// A parsed RRULE (the subset we expand).
enum { F_NONE, F_DAILY, F_WEEKLY, F_MONTHLY, F_YEARLY };

typedef struct {
    int freq, interval, count;  // count 0 = no COUNT
    bool has_until;
    itime_t until;
    uint16_t bymonth;            // bit m (1..12)
    int8_t bymonthday[31];
    int n_bymonthday;
    struct {
        int8_t n;   // 0 = every such weekday, else the nth (negative from the end)
        uint8_t wd; // 0 = Sunday
    } byday[28];
    int n_byday;
    int16_t bysetpos[16];
    int n_bysetpos;
    int wkst;
} rrule_t;

// False when the rule uses parts we can't expand (the event then shows its first occurrence).
bool ical_rule_parse(const char *s, rrule_t *r);
