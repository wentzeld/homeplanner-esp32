// .ics text -> events, times and zones (see ical_internal.h).
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ical_internal.h"

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
// Feeds can be megabytes: keep them in PSRAM, not the small internal RAM.
void *ical_realloc(void *p, size_t size) {
    if (!size) {
        free(p);
        return NULL;
    }
    void *r = heap_caps_realloc(p, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return r ? r : realloc(p, size);
}
#else
void *ical_realloc(void *p, size_t size) {
    if (!size) {
        free(p);
        return NULL;
    }
    return realloc(p, size);
}
#endif

#define MAX_LINE (256 * 1024)  // longer (unfolded) lines are cut
#define MAX_TEXT 4096          // stored summary/location/description length

static bool grow(void **p, size_t elem, uint32_t need, uint32_t *cap) {
    if (need <= *cap) return true;
    uint32_t n = *cap ? *cap * 2 : 64;
    while (n < need) n *= 2;
    void *q = ical_realloc(*p, (size_t)n * elem);
    if (!q) return false;
    *p = q;
    *cap = n;
    return true;
}

typedef struct {
    ical_cal_t *cal;
    bool oom;
} parser_t;

// Append a string to the pool; returns its offset (0 for "" or out of memory).
static uint32_t pool_add(parser_t *ps, const char *s, size_t n) {
    ical_cal_t *cal = ps->cal;
    if (!n) return 0;
    if (cal->pool_len + n + 1 > cal->pool_cap) {
        size_t cap = cal->pool_cap ? cal->pool_cap * 2 : 4096;
        while (cap < cal->pool_len + n + 1) cap *= 2;
        char *p = ical_realloc(cal->pool, cap);
        if (!p) {
            ps->oom = true;
            return 0;
        }
        cal->pool = p;
        cal->pool_cap = cap;
    }
    uint32_t off = (uint32_t)cal->pool_len;
    memcpy(cal->pool + off, s, n);
    cal->pool[off + n] = '\0';
    cal->pool_len += n + 1;
    return off;
}

// TEXT value: \n \, \; \\ escapes; trimmed and cut to max bytes (at a UTF-8 boundary).
static uint32_t pool_add_text(parser_t *ps, const char *v, size_t max) {
    size_t n = strlen(v);
    char *buf = malloc(n + 1);
    if (!buf) {
        ps->oom = true;
        return 0;
    }
    size_t o = 0;
    for (size_t i = 0; i < n; i++) {
        if (v[i] == '\\' && i + 1 < n) {
            char c = v[++i];
            buf[o++] = (c == 'n' || c == 'N') ? '\n' : c;
        } else if (v[i] != '\r') {
            buf[o++] = v[i];
        }
    }
    size_t a = 0;
    while (a < o && isspace((unsigned char)buf[a])) a++;
    while (o > a && isspace((unsigned char)buf[o - 1])) o--;
    if (o - a > max) {
        o = a + max;
        while (o > a && ((unsigned char)buf[o] & 0xC0) == 0x80) o--;
    }
    uint32_t off = pool_add(ps, buf + a, o - a);
    free(buf);
    return off;
}

// --- Content lines --------------------------------------------------------------------------

typedef struct {
    char *name;          // upper-cased
    char *params;        // ";KEY=VAL;KEY=\"VAL\"" (raw, or "" when none)
    char *value;
} cline_t;

static void split_line(char *line, cline_t *cl) {
    cl->name = line;
    cl->params = "";
    cl->value = "";
    char *p = line;
    while (*p && *p != ';' && *p != ':') {
        *p = (char)toupper((unsigned char)*p);
        p++;
    }
    if (*p == ';') {
        *p++ = '\0';
        cl->params = p;
        bool quoted = false;
        while (*p && (quoted || *p != ':')) {
            if (*p == '"') quoted = !quoted;
            p++;
        }
    }
    if (*p == ':') {
        *p++ = '\0';
        cl->value = p;
    }
}

// Value of parameter key (case-insensitive) without quotes, into out; false when absent.
static bool get_param(const char *params, const char *key, char *out, size_t size) {
    const char *p = params;
    size_t klen = strlen(key);
    while (*p) {
        const char *eq = strchr(p, '=');
        if (!eq) return false;
        bool match = (size_t)(eq - p) == klen && strncasecmp(p, key, klen) == 0;
        const char *v = eq + 1, *end;
        bool quoted = *v == '"';
        if (quoted) {
            v++;
            end = strchr(v, '"');
            if (!end) end = v + strlen(v);
        } else {
            end = v;
            while (*end && *end != ';') end++;
        }
        if (match) {
            size_t n = (size_t)(end - v) < size - 1 ? (size_t)(end - v) : size - 1;
            memcpy(out, v, n);
            out[n] = '\0';
            return true;
        }
        p = end;
        if (quoted && *p == '"') p++;
        if (*p == ';') p++;
    }
    return false;
}

// --- Zones ----------------------------------------------------------------------------------

typedef struct {
    bool present, has_rule;
    int offset_to;  // seconds east
    itime_t start;
    int month, week, wday;  // from RRULE BYMONTH / BYDAY=nDD
} vtz_part_t;

typedef struct {  // what a VTIMEZONE says: its newest STANDARD and DAYLIGHT parts
    char tzid[128];
    vtz_part_t part[2];  // 0 = STANDARD, 1 = DAYLIGHT
} vtz_t;

static int zone_index(parser_t *ps, const char *name) {
    ical_cal_t *cal = ps->cal;
    for (int i = 0; i < cal->n_zones; i++) {
        if (strcmp(ical_str(cal, cal->zones[i].name), name) == 0) return i;
    }
    uint32_t cap = (uint32_t)cal->cap_zones;
    if (!grow((void **)&cal->zones, sizeof(izone_t), (uint32_t)cal->n_zones + 1, &cap)) {
        ps->oom = true;
        return -1;
    }
    cal->cap_zones = (int)cap;
    izone_t *z = &cal->zones[cal->n_zones];
    memset(z, 0, sizeof *z);
    z->name = pool_add(ps, name, strlen(name));
    return cal->n_zones++;
}

static bool posix_for_name(const char *name, tz_rules_t *out) {
    const char *posix = tz_posix_for(name);
    if (!posix) posix = tz_posix_for(tz_windows_to_iana(name));
    if (!posix) {  // "/mozilla.org/20050126_1/Europe/London" and similar vendor prefixes
        for (const char *p = strchr(name, '/'); p && !posix; p = strchr(p + 1, '/')) {
            posix = tz_posix_for(p + 1);
        }
    }
    return posix && tz_parse(posix, out);
}

static void resolve_zones(ical_cal_t *cal, const vtz_t *vtz, int n_vtz) {
    for (int i = 0; i < cal->n_zones; i++) {
        izone_t *z = &cal->zones[i];
        const char *name = ical_str(cal, z->name);
        if (posix_for_name(name, &z->rules)) {
            z->known = true;
            continue;
        }
        for (int v = 0; v < n_vtz; v++) {  // the feed's own rules
            if (strcmp(vtz[v].tzid, name) != 0) continue;
            const vtz_part_t *std = &vtz[v].part[0], *dst = &vtz[v].part[1];
            if (!std->present && !dst->present) break;
            if (!std->present) std = dst;
            memset(&z->rules, 0, sizeof z->rules);
            z->rules.std_offset = std->offset_to;
            if (dst->present && std != dst && std->has_rule && dst->has_rule) {
                z->rules.has_dst = true;
                z->rules.dst_offset = dst->offset_to;
                z->rules.start = (tz_change_t){'M', dst->month, dst->week, dst->wday,
                                               dst->start.h * 3600 + dst->start.mi * 60 + dst->start.s};
                z->rules.end = (tz_change_t){'M', std->month, std->week, std->wday,
                                             std->start.h * 3600 + std->start.mi * 60 + std->start.s};
            }
            z->known = true;
            break;
        }
    }
}

// --- Values ---------------------------------------------------------------------------------

static int digits(const char *s, int n) {
    int v = 0;
    for (int i = 0; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) return -1;
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

// "20261005" / "20261005T170000" / "...Z"; tzid may be NULL.
static bool parse_time(parser_t *ps, const char *s, bool date_param, const char *tzid, itime_t *t) {
    memset(t, 0, sizeof *t);
    while (*s == ' ') s++;
    int y = digits(s, 4), mo = digits(s + 4, 2), d = digits(s + 6, 2);
    if (y < 0 || mo < 1 || mo > 12 || d < 1 || d > 31) return false;
    t->y = (int16_t)y;
    t->mo = (uint8_t)mo;
    t->d = (uint8_t)d;
    if (date_param || s[8] != 'T') {
        t->kind = T_DATE;
        return true;
    }
    int h = digits(s + 9, 2), mi = digits(s + 11, 2), sec = digits(s + 13, 2);
    if (h < 0 || h > 23 || mi < 0 || mi > 59 || sec < 0 || sec > 60) return false;
    t->h = (uint8_t)h;
    t->mi = (uint8_t)mi;
    t->s = (uint8_t)(sec > 59 ? 59 : sec);
    if (s[15] == 'Z' || s[15] == 'z') {
        t->kind = T_UTC;
    } else if (tzid && *tzid) {
        int z = zone_index(ps, tzid);
        t->kind = z >= 0 ? T_ZONED : T_FLOAT;
        t->zone = (int16_t)(z >= 0 ? z : 0);
    } else {
        t->kind = T_FLOAT;
    }
    return true;
}

// "P1D", "PT1H30M", "-PT15M", "P2W"
static bool parse_duration(const char *s, int32_t *out) {
    int sign = 1;
    if (*s == '+' || *s == '-') sign = *s++ == '-' ? -1 : 1;
    if (*s++ != 'P') return false;
    int64_t total = 0, n = 0;
    bool any = false;
    for (; *s; s++) {
        if (isdigit((unsigned char)*s)) {
            n = n * 10 + (*s - '0');
            any = true;
            continue;
        }
        switch (*s) {
            case 'W': total += n * 7 * 86400; break;
            case 'D': total += n * 86400; break;
            case 'H': total += n * 3600; break;
            case 'M': total += n * 60; break;
            case 'S': total += n; break;
            case 'T': break;
            default: return false;
        }
        n = 0;
    }
    *out = (int32_t)(sign * total);
    return any;
}

typedef struct {
    itime_t *v;
    uint32_t n, cap;
} tlist_t;

// Comma-separated times appended to list.
static void add_times(parser_t *ps, tlist_t *list, char *value, const char *params) {
    char tzid[128] = "", vtype[16] = "";
    get_param(params, "TZID", tzid, sizeof tzid);
    get_param(params, "VALUE", vtype, sizeof vtype);
    bool date_param = strcasecmp(vtype, "DATE") == 0;
    char *save = NULL;
    for (char *tok = strtok_r(value, ",", &save); tok; tok = strtok_r(NULL, ",", &save)) {
        itime_t t;
        if (!parse_time(ps, tok, date_param, tzid, &t)) continue;  // a PERIOD keeps its start
        if (!grow((void **)&list->v, sizeof(itime_t), list->n + 1, &list->cap)) {
            ps->oom = true;
            break;
        }
        list->v[list->n++] = t;
    }
}

static bool append_list(parser_t *ps, const tlist_t *list, uint32_t *first, uint32_t *count) {
    ical_cal_t *cal = ps->cal;
    *first = cal->n_times;
    *count = list->n;
    if (!list->n) return true;
    if (!grow((void **)&cal->times, sizeof(itime_t), cal->n_times + list->n, &cal->cap_times)) {
        ps->oom = true;
        return false;
    }
    memcpy(cal->times + cal->n_times, list->v, sizeof(itime_t) * list->n);
    cal->n_times += list->n;
    return true;
}

// --- The parser -----------------------------------------------------------------------------

static const ical_cal_t *g_sort_cal;  // qsort has no context argument; parse is not reentrant
static int cmp_uid(const void *a, const void *b) {
    const ical_cal_t *cal = g_sort_cal;
    int ia = *(const int *)a, ib = *(const int *)b;
    int c = strcmp(ical_str(cal, cal->events[ia].uid), ical_str(cal, cal->events[ib].uid));
    return c ? c : ia - ib;
}

static void parse_vtz_rrule(const char *v, int *month, int *week, int *wday, bool *ok) {
    static const char *DAYS[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};
    const char *bm = strstr(v, "BYMONTH="), *bd = strstr(v, "BYDAY=");
    *ok = false;
    if (!bm || !bd) return;
    *month = atoi(bm + 8);
    char *end;
    long n = strtol(bd + 6, &end, 10);
    for (int i = 0; i < 7; i++) {
        if (strncmp(end, DAYS[i], 2) == 0) *wday = i, *ok = true;
    }
    *week = n < 0 || n > 4 ? 5 : (int)(n ? n : 1);  // -1 (last) -> POSIX week 5
    if (*month < 1 || *month > 12) *ok = false;
}

static int utc_offset_value(const char *v) {  // "+0530" -> seconds
    int sign = *v == '-' ? -1 : 1;
    if (*v == '+' || *v == '-') v++;
    int h = digits(v, 2), m = digits(v + 2, 2), s = isdigit((unsigned char)v[4]) ? digits(v + 4, 2) : 0;
    return h < 0 || m < 0 ? 0 : sign * (h * 3600 + m * 60 + (s < 0 ? 0 : s));
}

ical_cal_t *ical_parse(const char *text, size_t len, ical_status_t *status) {
    ical_status_t dummy;
    if (!status) status = &dummy;
    *status = ICAL_NOT_CALENDAR;
    ical_cal_t *cal = calloc(1, sizeof *cal);
    if (!cal) {
        *status = ICAL_NO_MEMORY;
        return NULL;
    }
    parser_t ps = {.cal = cal};
    cal->pool = ical_realloc(NULL, 4096);
    if (!cal->pool) goto oom;
    cal->pool[0] = '\0';
    cal->pool_len = 1;
    cal->pool_cap = 4096;

    char *line = malloc(1024);
    size_t line_cap = 1024;
    vtz_t *vtz = NULL;
    int n_vtz = 0;
    if (!line) goto oom;

    const char *p = text, *end = text + len;
    if (len >= 3 && (unsigned char)p[0] == 0xEF && (unsigned char)p[1] == 0xBB && (unsigned char)p[2] == 0xBF) p += 3;
    bool seen_calendar = false, in_event = false;
    int nested = 0;  // components inside a VEVENT (VALARM...) or unknown ones
    int vtz_part = -1;
    bool in_vtz = false;
    ievent_t ev;
    tlist_t exdates = {0}, rdates = {0};
    vtz_part_t part;

    while (p < end && !ps.oom) {
        // One logical line: physical lines joined where the next starts with a space or tab.
        size_t n = 0;
        for (;;) {
            const char *eol = memchr(p, '\n', (size_t)(end - p));
            const char *stop = eol ? eol : end;
            size_t seg = (size_t)(stop - p);
            if (seg && p[seg - 1] == '\r') seg--;
            if (n + seg + 1 > line_cap && n < MAX_LINE) {
                size_t cap = line_cap;
                while (cap < n + seg + 1 && cap < MAX_LINE + 1) cap *= 2;
                char *nl = realloc(line, cap);
                if (!nl) {
                    ps.oom = true;
                    break;
                }
                line = nl;
                line_cap = cap;
            }
            size_t room = line_cap - 1 - n;
            memcpy(line + n, p, seg < room ? seg : room);
            n += seg < room ? seg : room;
            p = eol ? eol + 1 : end;
            if (p < end && (*p == ' ' || *p == '\t')) {
                p++;
                continue;
            }
            break;
        }
        line[n] = '\0';
        if (!n) continue;

        cline_t cl;
        split_line(line, &cl);
        if (!seen_calendar) {
            if (strcmp(cl.name, "BEGIN") == 0 && strcasecmp(cl.value, "VCALENDAR") == 0) {
                seen_calendar = true;
                continue;
            }
            break;  // the first line must start the calendar
        }

        if (strcmp(cl.name, "BEGIN") == 0) {
            if (in_event || in_vtz) {
                if (in_vtz && nested == 0 && (strcasecmp(cl.value, "STANDARD") == 0 || strcasecmp(cl.value, "DAYLIGHT") == 0)) {
                    vtz_part = strcasecmp(cl.value, "DAYLIGHT") == 0;
                    memset(&part, 0, sizeof part);
                } else {
                    nested++;
                }
            } else if (nested == 0 && strcasecmp(cl.value, "VEVENT") == 0) {
                in_event = true;
                memset(&ev, 0, sizeof ev);
                exdates.n = rdates.n = 0;
            } else if (nested == 0 && strcasecmp(cl.value, "VTIMEZONE") == 0) {
                vtz_t *nv = realloc(vtz, sizeof(vtz_t) * (n_vtz + 1));
                if (!nv) {
                    ps.oom = true;
                    break;
                }
                vtz = nv;
                memset(&vtz[n_vtz++], 0, sizeof(vtz_t));
                in_vtz = true;
                vtz_part = -1;
            } else {
                nested++;
            }
            continue;
        }
        if (strcmp(cl.name, "END") == 0) {
            if (nested > 0) {
                nested--;
            } else if (in_vtz && vtz_part >= 0) {
                // Keep the newest definition: a zone may list its history.
                vtz_part_t *kept = &vtz[n_vtz - 1].part[vtz_part];
                const itime_t *a = &part.start, *b = &kept->start;
                bool newer = !kept->present || a->y > b->y || (a->y == b->y && (a->mo > b->mo || (a->mo == b->mo && a->d >= b->d)));
                if (part.present && newer) *kept = part;
                vtz_part = -1;
            } else if (in_vtz) {
                in_vtz = false;
            } else if (in_event) {
                in_event = false;
                if (ev.start.kind == T_NONE) continue;  // DTSTART is required
                if (!append_list(&ps, &exdates, &ev.exdates, &ev.n_exdates) ||
                    !append_list(&ps, &rdates, &ev.rdates, &ev.n_rdates)) {
                    break;
                }
                rrule_t rule;
                if (ev.rrule && !ical_rule_parse(ical_str(cal, ev.rrule), &rule)) cal->unsupported++;
                uint32_t cap = (uint32_t)cal->cap_events;
                if (!grow((void **)&cal->events, sizeof(ievent_t), (uint32_t)cal->n_events + 1, &cap)) {
                    ps.oom = true;
                    break;
                }
                cal->cap_events = (int)cap;
                cal->events[cal->n_events++] = ev;
            }
            continue;
        }
        if (nested > 0) continue;

        if (in_vtz) {
            vtz_t *z = &vtz[n_vtz - 1];
            if (vtz_part < 0) {
                if (strcmp(cl.name, "TZID") == 0) {
                    uint32_t off = pool_add_text(&ps, cl.value, sizeof z->tzid - 1);
                    snprintf(z->tzid, sizeof z->tzid, "%s", ical_str(cal, off));
                }
                continue;
            }
            if (strcmp(cl.name, "DTSTART") == 0) {
                part.present = parse_time(&ps, cl.value, false, NULL, &part.start);
            } else if (strcmp(cl.name, "TZOFFSETTO") == 0) {
                part.offset_to = utc_offset_value(cl.value);
            } else if (strcmp(cl.name, "RRULE") == 0) {
                parse_vtz_rrule(cl.value, &part.month, &part.week, &part.wday, &part.has_rule);
            }
            continue;
        }
        if (!in_event) continue;

        char tzid[128] = "", vtype[16] = "";
        const char *name = cl.name;
        if (strcmp(name, "UID") == 0) {
            ev.uid = pool_add_text(&ps, cl.value, 1024);
        } else if (strcmp(name, "SUMMARY") == 0) {
            ev.summary = pool_add_text(&ps, cl.value, HP_TITLE_LEN - 1);
        } else if (strcmp(name, "LOCATION") == 0) {
            ev.location = pool_add_text(&ps, cl.value, HP_TEXT_LEN / 4 - 1);
        } else if (strcmp(name, "DESCRIPTION") == 0) {
            ev.description = pool_add_text(&ps, cl.value, MAX_TEXT);
        } else if (strcmp(name, "STATUS") == 0) {
            ev.cancelled = strncasecmp(cl.value, "CANCELLED", 9) == 0;
        } else if (strcmp(name, "SEQUENCE") == 0) {
            ev.sequence = atoi(cl.value);
        } else if (strcmp(name, "RRULE") == 0) {
            if (!ev.rrule) ev.rrule = pool_add(&ps, cl.value, strlen(cl.value));
        } else if (strcmp(name, "DURATION") == 0) {
            ev.has_duration = parse_duration(cl.value, &ev.duration);
        } else if (strcmp(name, "DTSTART") == 0 || strcmp(name, "DTEND") == 0 || strcmp(name, "RECURRENCE-ID") == 0) {
            get_param(cl.params, "TZID", tzid, sizeof tzid);
            get_param(cl.params, "VALUE", vtype, sizeof vtype);
            itime_t *t = name[2] == 'S' ? &ev.start : name[2] == 'E' ? &ev.end : &ev.rid;
            if (!parse_time(&ps, cl.value, strcasecmp(vtype, "DATE") == 0, tzid, t)) t->kind = T_NONE;
        } else if (strcmp(name, "EXDATE") == 0) {
            add_times(&ps, &exdates, cl.value, cl.params);
        } else if (strcmp(name, "RDATE") == 0) {
            add_times(&ps, &rdates, cl.value, cl.params);
        }
    }
    free(line);
    ical_realloc(exdates.v, 0);
    ical_realloc(rdates.v, 0);
    if (ps.oom) {
        free(vtz);
        goto oom;
    }
    if (!seen_calendar) {
        free(vtz);
        ical_free(cal);
        *status = ICAL_NOT_CALENDAR;
        return NULL;
    }
    resolve_zones(cal, vtz, n_vtz);
    free(vtz);

    cal->by_uid = ical_realloc(NULL, sizeof(int) * (size_t)(cal->n_events ? cal->n_events : 1));
    if (!cal->by_uid) goto oom;
    for (int i = 0; i < cal->n_events; i++) cal->by_uid[i] = i;
    g_sort_cal = cal;
    qsort(cal->by_uid, (size_t)cal->n_events, sizeof(int), cmp_uid);
    *status = ICAL_OK;
    return cal;

oom:
    ical_free(cal);
    *status = ICAL_NO_MEMORY;
    return NULL;
}

void ical_free(ical_cal_t *cal) {
    if (!cal) return;
    ical_realloc(cal->events, 0);
    ical_realloc(cal->times, 0);
    ical_realloc(cal->zones, 0);
    ical_realloc(cal->pool, 0);
    ical_realloc(cal->by_uid, 0);
    free(cal);
}

int ical_event_count(const ical_cal_t *cal) { return cal ? cal->n_events : 0; }
int ical_unsupported_count(const ical_cal_t *cal) { return cal ? cal->unsupported : 0; }
