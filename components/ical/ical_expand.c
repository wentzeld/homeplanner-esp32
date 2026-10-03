// Repeating events -> occurrences, following RFC 5545 as python-dateutil / recurring_ical_events
// read it (the Pi version's behaviour): DTSTART always counts as an occurrence, COUNT counts rule
// matches, EXDATE/RECURRENCE-ID match by UTC instant or wall time, a DATE EXDATE removes the day.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ical_internal.h"

#define MAX_PERIODS 200000  // safety net against rules that never match

// --- RRULE text ---------------------------------------------------------------------------------

static int weekday_code(const char *s) {
    static const char *DAYS[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    for (int i = 0; i < 7; i++) {
        if (strncasecmp(s, DAYS[i], 2) == 0) return i;
    }
    return -1;
}

static bool parse_until(const char *s, itime_t *t) {
    memset(t, 0, sizeof *t);
    int v[6] = {0};
    if (sscanf(s, "%4d%2d%2d", &v[0], &v[1], &v[2]) != 3) return false;
    t->y = (int16_t)v[0];
    t->mo = (uint8_t)v[1];
    t->d = (uint8_t)v[2];
    t->kind = T_DATE;
    if (s[8] == 'T' && sscanf(s + 9, "%2d%2d%2d", &v[3], &v[4], &v[5]) == 3) {
        t->h = (uint8_t)v[3];
        t->mi = (uint8_t)v[4];
        t->s = (uint8_t)v[5];
        t->kind = (s[15] == 'Z' || s[15] == 'z') ? T_UTC : T_FLOAT;
    }
    return t->mo >= 1 && t->mo <= 12 && t->d >= 1 && t->d <= 31;
}

bool ical_rule_parse(const char *s, rrule_t *r) {
    memset(r, 0, sizeof *r);
    r->interval = 1;
    r->wkst = 1;  // Monday
    char buf[512];
    snprintf(buf, sizeof buf, "%s", s);
    bool ok = true;
    char *save = NULL;
    for (char *part = strtok_r(buf, ";", &save); part; part = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(part, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = part;
        char *val = eq + 1, *save2 = NULL;
        if (strcasecmp(key, "FREQ") == 0) {
            r->freq = strcasecmp(val, "DAILY") == 0     ? F_DAILY
                      : strcasecmp(val, "WEEKLY") == 0  ? F_WEEKLY
                      : strcasecmp(val, "MONTHLY") == 0 ? F_MONTHLY
                      : strcasecmp(val, "YEARLY") == 0  ? F_YEARLY
                                                        : F_NONE;
        } else if (strcasecmp(key, "INTERVAL") == 0) {
            r->interval = atoi(val) > 0 ? atoi(val) : 1;
        } else if (strcasecmp(key, "COUNT") == 0) {
            r->count = atoi(val) > 0 ? atoi(val) : 0;  // a negative COUNT is ignored, as on the Pi
        } else if (strcasecmp(key, "UNTIL") == 0) {
            r->has_until = parse_until(val, &r->until);
        } else if (strcasecmp(key, "WKST") == 0) {
            int wd = weekday_code(val);
            if (wd >= 0) r->wkst = wd;
        } else if (strcasecmp(key, "BYMONTH") == 0) {
            for (char *t = strtok_r(val, ",", &save2); t; t = strtok_r(NULL, ",", &save2)) {
                int m = atoi(t);
                if (m >= 1 && m <= 12) r->bymonth |= (uint16_t)(1u << m);
                else ok = false;
            }
        } else if (strcasecmp(key, "BYMONTHDAY") == 0) {
            for (char *t = strtok_r(val, ",", &save2); t; t = strtok_r(NULL, ",", &save2)) {
                int d = atoi(t);
                if (d && d >= -31 && d <= 31 && r->n_bymonthday < 31) r->bymonthday[r->n_bymonthday++] = (int8_t)d;
                else ok = false;
            }
        } else if (strcasecmp(key, "BYDAY") == 0) {
            for (char *t = strtok_r(val, ",", &save2); t; t = strtok_r(NULL, ",", &save2)) {
                char *end;
                long n = strtol(t, &end, 10);
                int wd = weekday_code(end);
                if (wd < 0 || n < -53 || n > 53 || r->n_byday >= 28) {
                    ok = false;
                    continue;
                }
                r->byday[r->n_byday].n = (int8_t)n;
                r->byday[r->n_byday++].wd = (uint8_t)wd;
            }
        } else if (strcasecmp(key, "BYSETPOS") == 0) {
            for (char *t = strtok_r(val, ",", &save2); t; t = strtok_r(NULL, ",", &save2)) {
                int p = atoi(t);
                if (p && p >= -366 && p <= 366 && r->n_bysetpos < 16) r->bysetpos[r->n_bysetpos++] = (int16_t)p;
                else ok = false;
            }
        } else if (strncasecmp(key, "BY", 2) == 0) {
            ok = false;  // BYWEEKNO, BYYEARDAY, BYHOUR, BYMINUTE, BYSECOND, BYEASTER
        }
    }
    return ok && r->freq != F_NONE;
}

// --- Times --------------------------------------------------------------------------------------

typedef struct {  // a start time in its own frame (kind/zone) as wall-clock seconds
    uint8_t kind;
    int16_t zone;
    int64_t wall;
} stime_t;

typedef struct {
    const ical_cal_t *cal;
    tz_rules_t local;
    int64_t from, to;  // UTC window
    ical_visit_t visit;
    void *ctx;
    int visited;
    bool stop;
} xctx_t;

static int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

static int64_t days_of(int y, int m, int d) { return hp_days_from_civil((hp_date_t){y, m, d}); }

static int64_t wall_of(const itime_t *t) { return days_of(t->y, t->mo, t->d) * 86400 + t->h * 3600 + t->mi * 60 + t->s; }

static bool aware(const xctx_t *x, uint8_t kind, int16_t zone) {
    return kind == T_UTC || (kind == T_ZONED && x->cal->zones[zone].known);
}

static int64_t to_utc(const xctx_t *x, uint8_t kind, int16_t zone, int64_t wall) {
    if (kind == T_UTC) return wall;
    const tz_rules_t *rules = (kind == T_ZONED && x->cal->zones[zone].known) ? &x->cal->zones[zone].rules : &x->local;
    int64_t days = floor_div(wall, 86400), secs = wall - days * 86400;
    hp_date_t d = hp_civil_from_days(days);
    return tz_rules_local_to_utc(rules, d.y, d.m, d.d, (int)(secs / 3600), (int)(secs / 60 % 60), (int)(secs % 60));
}

static int64_t itime_utc(const xctx_t *x, const itime_t *t) { return to_utc(x, t->kind, t->zone, wall_of(t)); }

// Recurrence ids, as recurring_ical_events compares them: an aware time is known by its UTC
// instant and by its wall time, a floating time or a date only by its wall time.
typedef struct {
    int64_t v[2];
    int n;
} ids_t;

static ids_t ids_of(const xctx_t *x, uint8_t kind, int16_t zone, int64_t wall) {
    ids_t ids = {{wall, 0}, 1};
    if (aware(x, kind, zone)) ids.v[ids.n++] = to_utc(x, kind, zone, wall);
    return ids;
}

static bool ids_meet(const ids_t *a, const ids_t *b) {
    for (int i = 0; i < a->n; i++) {
        for (int j = 0; j < b->n; j++) {
            if (a->v[i] == b->v[j]) return true;
        }
    }
    return false;
}

// --- Occurrences ---------------------------------------------------------------------------------

typedef struct {
    bool all_day;
    int64_t dur;       // seconds of wall time added to the start (all-day: whole days)
} span_t;

static span_t span_of(const xctx_t *x, const ievent_t *e) {
    span_t s = {e->start.kind == T_DATE, 0};
    if (s.all_day) {
        int64_t days = 1;
        if (e->end.kind == T_DATE) days = days_of(e->end.y, e->end.mo, e->end.d) - days_of(e->start.y, e->start.mo, e->start.d);
        else if (e->has_duration) days = e->duration / 86400;
        s.dur = (days > 0 ? days : 1) * 86400;  // an all-day event lasts at least its day
        return s;
    }
    if (e->end.kind != T_NONE) {
        bool same_frame = e->end.kind == T_DATE || e->end.kind == T_FLOAT || e->start.kind == T_FLOAT ||
                          (e->end.kind == e->start.kind && (e->end.kind != T_ZONED || e->end.zone == e->start.zone));
        s.dur = same_frame ? wall_of(&e->end) - wall_of(&e->start) : itime_utc(x, &e->end) - itime_utc(x, &e->start);
    } else if (e->has_duration) {
        s.dur = e->duration;
    }
    return s;
}

static void emit(xctx_t *x, int event, const stime_t *st, const span_t *sp) {
    if (x->stop) return;
    ical_occurrence_t occ = {.event = event, .all_day = sp->all_day};
    occ.start = to_utc(x, st->kind, st->zone, st->wall);
    occ.end = to_utc(x, st->kind, st->zone, st->wall + sp->dur);
    if (occ.end < occ.start) occ.end = occ.start;
    int64_t probe_end = occ.end > occ.start ? occ.end : occ.start + 1;  // zero-length: its start's day
    if (occ.start >= x->to || probe_end <= x->from) return;
    x->visited++;
    if (!x->visit(x->cal, &occ, x->ctx)) x->stop = true;
}

static void emit_single(xctx_t *x, int idx) {
    const ievent_t *e = &x->cal->events[idx];
    if (e->cancelled) return;
    stime_t st = {e->start.kind, e->start.zone, wall_of(&e->start)};
    span_t sp = span_of(x, e);
    emit(x, idx, &st, &sp);
}

// --- Rule expansion -----------------------------------------------------------------------------

typedef struct {
    const ievent_t *core;
    int core_idx;
    const int *mods;  // modification event indexes for the same UID
    int n_mods;
    span_t span;
    int64_t *extra_utc;  // DTSTART + RDATEs, already emitted
    int n_extra;
} series_t;

static bool excluded(const xctx_t *x, const series_t *s, const stime_t *st) {
    const ical_cal_t *cal = x->cal;
    const ievent_t *e = s->core;
    ids_t ids = ids_of(x, st->kind, st->zone, st->wall);
    int64_t day = floor_div(st->wall, 86400);
    for (uint32_t i = 0; i < e->n_exdates; i++) {
        const itime_t *ex = &cal->times[e->exdates + i];
        if (ex->kind == T_DATE && days_of(ex->y, ex->mo, ex->d) == day) return true;
        ids_t exi = ids_of(x, ex->kind, ex->zone, wall_of(ex));
        if (ids_meet(&ids, &exi)) return true;
    }
    for (int i = 0; i < s->n_mods; i++) {  // replaced by a modified copy (emitted on its own)
        const itime_t *rid = &cal->events[s->mods[i]].rid;
        ids_t ri = ids_of(x, rid->kind, rid->zone, wall_of(rid));
        if (ids_meet(&ids, &ri)) return true;
    }
    return false;
}

static void emit_start(xctx_t *x, series_t *s, const stime_t *st, bool from_rule) {
    if (from_rule) {
        int64_t utc = to_utc(x, st->kind, st->zone, st->wall);
        for (int i = 0; i < s->n_extra; i++) {
            if (s->extra_utc[i] == utc) return;
        }
    }
    if (excluded(x, s, st)) return;
    emit(x, s->core_idx, st, &s->span);
}

// Is a start (in the series' frame) after UNTIL? Compared like dateutil after the Pi's fixes:
// all-day by date, floating by wall time, otherwise UNTIL is a UTC instant.
static bool after_until(const xctx_t *x, const rrule_t *r, const stime_t *st) {
    if (!r->has_until) return false;
    if (st->kind == T_DATE) return floor_div(st->wall, 86400) > days_of(r->until.y, r->until.mo, r->until.d);
    int64_t until_wall = wall_of(&r->until);
    if (!aware(x, st->kind, st->zone)) return st->wall > until_wall;
    return to_utc(x, st->kind, st->zone, st->wall) > until_wall;
}

static int month_len(int y, int m) {
    static const int LEN[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    bool leap = (y % 4 == 0 && y % 100 != 0) || y % 400 == 0;
    return m == 2 && leap ? 29 : LEN[m - 1];
}

static int wd_of(int64_t day) { return (int)(((day % 7) + 7 + 4) % 7); }  // 1970-01-01 was a Thursday

static bool day_matches_monthday(const rrule_t *r, int64_t day) {
    hp_date_t d = hp_civil_from_days(day);
    int len = month_len(d.y, d.m);
    for (int i = 0; i < r->n_bymonthday; i++) {
        int md = r->bymonthday[i] > 0 ? r->bymonthday[i] : len + r->bymonthday[i] + 1;
        if (md == d.d) return true;
    }
    return false;
}

static bool day_matches_weekday(const rrule_t *r, int64_t day) {
    int wd = wd_of(day);
    for (int i = 0; i < r->n_byday; i++) {
        if (r->byday[i].wd == wd) return true;
    }
    return false;
}

static bool month_ok(const rrule_t *r, int m) { return !r->bymonth || (r->bymonth & (1u << m)); }

// nth weekday wd in [first, last] (days); n < 0 counts from the end. -1 when there is none.
static int64_t nth_weekday(int64_t first, int64_t last, int wd, int n) {
    if (n > 0) {
        int64_t d = first + (wd - wd_of(first) + 7) % 7 + 7 * (n - 1);
        return d <= last ? d : -1;
    }
    int64_t d = last - (wd_of(last) - wd + 7) % 7 + 7 * (n + 1);
    return d >= first ? d : -1;
}

typedef struct {
    int32_t d[372];
    int n;
} dayset_t;

static void add_day(dayset_t *ds, int64_t day) {
    if (ds->n < 372) ds->d[ds->n++] = (int32_t)day;
}

static int cmp_i32(const void *a, const void *b) {
    int32_t x = *(const int32_t *)a, y = *(const int32_t *)b;
    return (x > y) - (x < y);
}

static void sort_unique(dayset_t *ds) {
    qsort(ds->d, (size_t)ds->n, sizeof ds->d[0], cmp_i32);
    int o = 0;
    for (int i = 0; i < ds->n; i++) {
        if (!o || ds->d[o - 1] != ds->d[i]) ds->d[o++] = ds->d[i];
    }
    ds->n = o;
}

// Days in [first, last] chosen by BYMONTHDAY/BYDAY (intersection when both); ordinals count
// within [first, last] (a month, or the year).
static void days_in_range(const rrule_t *r, int64_t first, int64_t last, dayset_t *ds) {
    for (int64_t day = first; day <= last; day++) {
        bool md = r->n_bymonthday && day_matches_monthday(r, day);
        bool bd = false;
        for (int i = 0; i < r->n_byday && !bd; i++) {
            if (r->byday[i].n == 0) bd = wd_of(day) == r->byday[i].wd;
            else bd = nth_weekday(first, last, r->byday[i].wd, r->byday[i].n) == day;
        }
        if (r->n_bymonthday && r->n_byday ? (md && bd) : r->n_bymonthday ? md : bd) add_day(ds, day);
    }
}

static void month_days(const rrule_t *r, int y, int m, dayset_t *ds) {
    int64_t first = days_of(y, m, 1);
    days_in_range(r, first, first + month_len(y, m) - 1, ds);
}

static void apply_setpos(const rrule_t *r, dayset_t *ds) {
    if (!r->n_bysetpos || !ds->n) return;
    dayset_t out = {.n = 0};
    for (int i = 0; i < r->n_bysetpos; i++) {
        int p = r->bysetpos[i], k = p > 0 ? p - 1 : ds->n + p;
        if (k >= 0 && k < ds->n) add_day(&out, ds->d[k]);
    }
    sort_unique(&out);
    *ds = out;
}

static void expand_rule(xctx_t *x, series_t *s, const rrule_t *rule) {
    rrule_t r = *rule;
    const ievent_t *e = s->core;
    stime_t st0 = {e->start.kind, e->start.zone, wall_of(&e->start)};
    int64_t day0 = floor_div(st0.wall, 86400), tod = st0.wall - day0 * 86400;
    hp_date_t d0 = hp_civil_from_days(day0);
    if (!r.n_bymonthday && !r.n_byday) {  // dateutil's defaults from DTSTART
        if (r.freq == F_YEARLY) {
            if (!r.bymonth) r.bymonth = (uint16_t)(1u << d0.m);
            r.bymonthday[r.n_bymonthday++] = (int8_t)d0.d;
        } else if (r.freq == F_MONTHLY) {
            r.bymonthday[r.n_bymonthday++] = (int8_t)d0.d;
        } else if (r.freq == F_WEEKLY) {
            r.byday[r.n_byday].n = 0;
            r.byday[r.n_byday++].wd = (uint8_t)wd_of(day0);
        }
    }
    bool year_ordinals = false;
    for (int i = 0; i < r.n_byday; i++) year_ordinals |= r.byday[i].n != 0;
    year_ordinals = year_ordinals && r.freq == F_YEARLY && !r.bymonth;

    // Days we may skip: whole periods ending before the window (not with COUNT: those count).
    int64_t lo_day = floor_div(x->from - s->span.dur, 86400) - 3, hi_day = floor_div(x->to, 86400) + 3;
    int64_t week0 = day0 - (wd_of(day0) - r.wkst + 7) % 7;
    int64_t k = 0, I = r.interval;
    if (!r.count) {
        int64_t skip = 0;
        switch (r.freq) {
            case F_DAILY: skip = floor_div(lo_day - day0, I); break;
            case F_WEEKLY: skip = floor_div(floor_div(lo_day - week0, 7), I) - 1; break;
            case F_MONTHLY: {
                hp_date_t lo = hp_civil_from_days(lo_day);
                skip = floor_div((lo.y * 12 + lo.m) - (d0.y * 12 + d0.m), I) - 1;
                break;
            }
            case F_YEARLY: skip = floor_div(hp_civil_from_days(lo_day).y - d0.y, I) - 1; break;
        }
        if (skip > 0) k = skip;
    }

    int generated = 0;
    for (int periods = 0; periods < MAX_PERIODS && !x->stop; periods++, k++) {
        dayset_t ds = {.n = 0};
        int64_t period_first;
        if (r.freq == F_DAILY) {
            int64_t day = day0 + k * I;
            period_first = day;
            hp_date_t d = hp_civil_from_days(day);
            if (month_ok(&r, d.m) && (!r.n_bymonthday || day_matches_monthday(&r, day)) &&
                (!r.n_byday || day_matches_weekday(&r, day))) {
                add_day(&ds, day);
            }
        } else if (r.freq == F_WEEKLY) {
            period_first = week0 + 7 * k * I;
            for (int64_t day = period_first; day < period_first + 7; day++) {
                if (month_ok(&r, hp_civil_from_days(day).m) && day_matches_weekday(&r, day) &&
                    (!r.n_bymonthday || day_matches_monthday(&r, day))) {
                    add_day(&ds, day);
                }
            }
        } else if (r.freq == F_MONTHLY) {
            int64_t mi = (int64_t)d0.y * 12 + (d0.m - 1) + k * I;
            int y = (int)floor_div(mi, 12), m = (int)(mi - floor_div(mi, 12) * 12) + 1;
            period_first = days_of(y, m, 1);
            if (month_ok(&r, m)) month_days(&r, y, m, &ds);
        } else {
            int y = (int)(d0.y + k * I);
            period_first = days_of(y, 1, 1);
            if (year_ordinals) {
                days_in_range(&r, period_first, days_of(y, 12, 31), &ds);
            } else {
                for (int m = 1; m <= 12; m++) {
                    if (month_ok(&r, m)) month_days(&r, y, m, &ds);
                }
            }
        }
        if (period_first > hi_day) return;
        apply_setpos(&r, &ds);
        for (int i = 0; i < ds.n; i++) {
            stime_t st = {st0.kind, st0.zone, (int64_t)ds.d[i] * 86400 + tod};
            if (st.wall < st0.wall) continue;
            if (after_until(x, &r, &st)) return;
            emit_start(x, s, &st, true);
            if (r.count && ++generated >= r.count) return;
        }
    }
}

static void expand_series(xctx_t *x, series_t *s) {
    const ical_cal_t *cal = x->cal;
    const ievent_t *e = s->core;
    s->span = span_of(x, e);
    rrule_t rule;
    bool has_rule = e->rrule && ical_rule_parse(ical_str(cal, e->rrule), &rule);
    int64_t extra[64];
    s->extra_utc = extra;
    s->n_extra = 0;

    // DTSTART (unless past UNTIL) and RDATEs are occurrences of their own.
    stime_t st0 = {e->start.kind, e->start.zone, wall_of(&e->start)};
    bool cancelled = e->cancelled;
    if (!(has_rule && after_until(x, &rule, &st0))) {
        if (!cancelled) emit_start(x, s, &st0, false);
        extra[s->n_extra++] = to_utc(x, st0.kind, st0.zone, st0.wall);
    }
    for (uint32_t i = 0; i < e->n_rdates; i++) {
        const itime_t *rd = &cal->times[e->rdates + i];
        stime_t st = {rd->kind, rd->zone, wall_of(rd)};
        if (rd->kind == T_FLOAT && e->start.kind != T_FLOAT && e->start.kind != T_DATE) st.kind = e->start.kind, st.zone = e->start.zone;
        if (rd->kind == T_DATE && e->start.kind != T_DATE) st.kind = e->start.kind, st.zone = e->start.zone;
        int64_t utc = to_utc(x, st.kind, st.zone, st.wall);
        bool dup = false;
        for (int j = 0; j < s->n_extra && !dup; j++) dup = extra[j] == utc;
        if (dup) continue;
        if (!cancelled) emit_start(x, s, &st, false);
        if (s->n_extra < 64) extra[s->n_extra++] = utc;
    }
    if (has_rule && !cancelled) expand_rule(x, s, &rule);
}

// --- Public ---------------------------------------------------------------------------------------

int ical_expand(const ical_cal_t *cal, const char *local_posix, hp_date_t first, int days,
                ical_visit_t visit, void *ctx) {
    if (!cal || !visit || days <= 0) return 0;
    xctx_t x = {.cal = cal, .visit = visit, .ctx = ctx};
    if (!tz_parse(local_posix, &x.local)) tz_parse("UTC0", &x.local);
    x.from = tz_rules_local_to_utc(&x.local, first.y, first.m, first.d, 0, 0, 0);
    hp_date_t last = hp_date_add(first, days);
    x.to = tz_rules_local_to_utc(&x.local, last.y, last.m, last.d, 0, 0, 0);

    int mods[64];
    for (int i = 0; i < cal->n_events && !x.stop;) {
        // One UID at a time (events without a UID stand alone).
        const char *uid = ical_str(cal, cal->events[cal->by_uid[i]].uid);
        int j = i + 1;
        while (*uid && j < cal->n_events && strcmp(uid, ical_str(cal, cal->events[cal->by_uid[j]].uid)) == 0) j++;

        int core = -1, n_mods = 0;
        for (int g = i; g < j; g++) {
            int idx = cal->by_uid[g];
            const ievent_t *e = &cal->events[idx];
            if (e->rid.kind == T_NONE) {  // the series; the highest SEQUENCE wins, first on a tie
                if (core < 0 || e->sequence > cal->events[core].sequence) core = idx;
                continue;
            }
            ids_t ri = ids_of(&x, e->rid.kind, e->rid.zone, wall_of(&e->rid));
            int same = -1;
            for (int m = 0; m < n_mods && same < 0; m++) {
                const itime_t *o = &cal->events[mods[m]].rid;
                ids_t oi = ids_of(&x, o->kind, o->zone, wall_of(o));
                if (ids_meet(&ri, &oi)) same = m;
            }
            if (same >= 0) {
                if (e->sequence > cal->events[mods[same]].sequence) mods[same] = idx;
            } else if (n_mods < 64) {
                mods[n_mods++] = idx;
            }
        }

        series_t s = {.core = core >= 0 ? &cal->events[core] : NULL, .core_idx = core, .mods = mods, .n_mods = n_mods};
        if (s.core) expand_series(&x, &s);
        for (int m = 0; m < n_mods && !x.stop; m++) {  // modified occurrences, unless excluded
            const ievent_t *e = &cal->events[mods[m]];
            bool gone = false;
            if (s.core) {
                ids_t ri = ids_of(&x, e->rid.kind, e->rid.zone, wall_of(&e->rid));
                for (uint32_t k = 0; k < s.core->n_exdates && !gone; k++) {
                    const itime_t *ex = &cal->times[s.core->exdates + k];
                    ids_t exi = ids_of(&x, ex->kind, ex->zone, wall_of(ex));
                    gone = ids_meet(&ri, &exi);
                }
            }
            if (!gone) emit_single(&x, mods[m]);
        }
        i = j;
    }
    return x.visited;
}

void ical_occurrence_to_event(const ical_cal_t *cal, const ical_occurrence_t *occ, const char *cal_id,
                              const char *cal_name, const char *color, hp_event_t *out) {
    const ievent_t *e = &cal->events[occ->event];
    memset(out, 0, sizeof *out);
    snprintf(out->id, sizeof out->id, "ext-%s-%lld-%.100s", cal_id, (long long)occ->start, ical_str(cal, e->uid));
    const char *title = ical_str(cal, e->summary);
    snprintf(out->title, sizeof out->title, "%s", *title ? title : "(No title)");
    out->start = occ->start;
    out->end = occ->end;
    out->all_day = occ->all_day;
    snprintf(out->location, sizeof out->location, "%s", ical_str(cal, e->location));
    const char *desc = ical_str(cal, e->description);
    if (strchr(desc, '<')) hp_html_to_text(desc, out->description, sizeof out->description);
    else snprintf(out->description, sizeof out->description, "%s", desc);
    snprintf(out->calendar, sizeof out->calendar, "%s", cal_name ? cal_name : "");
    snprintf(out->color, sizeof out->color, "%s", color ? color : HP_FAMILY_COLOR);
    out->editable = false;
    out->owned = false;
    out->recurring = e->rrule || e->rid.kind != T_NONE || e->n_rdates;
}
