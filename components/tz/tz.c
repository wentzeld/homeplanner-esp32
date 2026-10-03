#include "tz.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

const char *tz_posix_for(const char *name) {
    if (!name) return NULL;
    int lo = 0, hi = TZ_TABLE_COUNT - 1;  // table is sorted by name
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        int c = strcmp(name, TZ_TABLE[mid].name);
        if (c == 0) return TZ_TABLE[mid].posix;
        if (c < 0) hi = mid - 1;
        else lo = mid + 1;
    }
    return NULL;
}

// --- Windows names (CLDR windowsZones.xml, territory "001") ---------------------------------------

static const struct { const char *windows, *iana; } WINDOWS[] = {
    {"AUS Central Standard Time", "Australia/Darwin"},
    {"AUS Eastern Standard Time", "Australia/Sydney"},
    {"Afghanistan Standard Time", "Asia/Kabul"},
    {"Alaskan Standard Time", "America/Anchorage"},
    {"Arab Standard Time", "Asia/Riyadh"},
    {"Arabian Standard Time", "Asia/Dubai"},
    {"Arabic Standard Time", "Asia/Baghdad"},
    {"Argentina Standard Time", "America/Argentina/Buenos_Aires"},
    {"Atlantic Standard Time", "America/Halifax"},
    {"Azores Standard Time", "Atlantic/Azores"},
    {"Canada Central Standard Time", "America/Regina"},
    {"Cen. Australia Standard Time", "Australia/Adelaide"},
    {"Central America Standard Time", "America/Guatemala"},
    {"Central Asia Standard Time", "Asia/Almaty"},
    {"Central Europe Standard Time", "Europe/Budapest"},
    {"Central European Standard Time", "Europe/Warsaw"},
    {"Central Pacific Standard Time", "Pacific/Guadalcanal"},
    {"Central Standard Time", "America/Chicago"},
    {"Central Standard Time (Mexico)", "America/Mexico_City"},
    {"China Standard Time", "Asia/Shanghai"},
    {"E. Africa Standard Time", "Africa/Nairobi"},
    {"E. Australia Standard Time", "Australia/Brisbane"},
    {"E. Europe Standard Time", "Europe/Chisinau"},
    {"E. South America Standard Time", "America/Sao_Paulo"},
    {"Eastern Standard Time", "America/New_York"},
    {"Egypt Standard Time", "Africa/Cairo"},
    {"FLE Standard Time", "Europe/Kyiv"},
    {"GMT Standard Time", "Europe/London"},
    {"GTB Standard Time", "Europe/Bucharest"},
    {"Greenwich Standard Time", "Atlantic/Reykjavik"},
    {"Hawaiian Standard Time", "Pacific/Honolulu"},
    {"India Standard Time", "Asia/Kolkata"},
    {"Iran Standard Time", "Asia/Tehran"},
    {"Israel Standard Time", "Asia/Jerusalem"},
    {"Korea Standard Time", "Asia/Seoul"},
    {"Mountain Standard Time", "America/Denver"},
    {"Mountain Standard Time (Mexico)", "America/Mazatlan"},
    {"Myanmar Standard Time", "Asia/Yangon"},
    {"N. Central Asia Standard Time", "Asia/Novosibirsk"},
    {"Nepal Standard Time", "Asia/Kathmandu"},
    {"New Zealand Standard Time", "Pacific/Auckland"},
    {"Newfoundland Standard Time", "America/St_Johns"},
    {"North Asia East Standard Time", "Asia/Irkutsk"},
    {"North Asia Standard Time", "Asia/Krasnoyarsk"},
    {"Pacific SA Standard Time", "America/Santiago"},
    {"Pacific Standard Time", "America/Los_Angeles"},
    {"Pacific Standard Time (Mexico)", "America/Tijuana"},
    {"Romance Standard Time", "Europe/Paris"},
    {"Russian Standard Time", "Europe/Moscow"},
    {"SA Eastern Standard Time", "America/Cayenne"},
    {"SA Pacific Standard Time", "America/Bogota"},
    {"SA Western Standard Time", "America/La_Paz"},
    {"SE Asia Standard Time", "Asia/Bangkok"},
    {"Singapore Standard Time", "Asia/Singapore"},
    {"South Africa Standard Time", "Africa/Johannesburg"},
    {"Sri Lanka Standard Time", "Asia/Colombo"},
    {"Taipei Standard Time", "Asia/Taipei"},
    {"Tasmania Standard Time", "Australia/Hobart"},
    {"Tokyo Standard Time", "Asia/Tokyo"},
    {"Tonga Standard Time", "Pacific/Tongatapu"},
    {"Turkey Standard Time", "Europe/Istanbul"},
    {"US Eastern Standard Time", "America/Indiana/Indianapolis"},
    {"US Mountain Standard Time", "America/Phoenix"},
    {"UTC", "UTC"},
    {"Ulaanbaatar Standard Time", "Asia/Ulaanbaatar"},
    {"Venezuela Standard Time", "America/Caracas"},
    {"Vladivostok Standard Time", "Asia/Vladivostok"},
    {"W. Australia Standard Time", "Australia/Perth"},
    {"W. Central Africa Standard Time", "Africa/Lagos"},
    {"W. Europe Standard Time", "Europe/Berlin"},
    {"West Asia Standard Time", "Asia/Tashkent"},
    {"West Pacific Standard Time", "Pacific/Port_Moresby"},
    {"Yakutsk Standard Time", "Asia/Yakutsk"},
};

const char *tz_windows_to_iana(const char *windows_name) {
    if (!windows_name) return NULL;
    for (size_t i = 0; i < sizeof WINDOWS / sizeof WINDOWS[0]; i++) {
        if (strcmp(windows_name, WINDOWS[i].windows) == 0) return WINDOWS[i].iana;
    }
    return NULL;
}

// --- POSIX TZ rules ------------------------------------------------------------------------------

static int64_t days_from_civil(int y, int m, int d) {  // days since 1970-01-01
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static int year_of_days(int64_t days) {
    days += 719468;
    int64_t era = (days >= 0 ? days : days - 146096) / 146097;
    int64_t doe = days - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    return (int)(yoe + era * 400 + (mp >= 10));
}

static int64_t floor_div(int64_t a, int64_t b) { return a / b - ((a % b != 0) && ((a < 0) != (b < 0))); }

static bool is_leap(int y) { return (y % 4 == 0 && y % 100 != 0) || y % 400 == 0; }

static const char *parse_name(const char *p) {
    if (*p == '<') {
        const char *e = strchr(p, '>');
        return e && e > p + 1 ? e + 1 : NULL;
    }
    const char *s = p;
    while (isalpha((unsigned char)*p)) p++;
    return p - s >= 3 ? p : NULL;
}

// [+-]hh[:mm[:ss]] -> seconds; NULL when there is no number
static const char *parse_hms(const char *p, int *out) {
    int sign = 1;
    if (*p == '+' || *p == '-') sign = *p++ == '-' ? -1 : 1;
    if (!isdigit((unsigned char)*p)) return NULL;
    int parts[3] = {0, 0, 0};
    for (int i = 0; i < 3; i++) {
        char *end;
        parts[i] = (int)strtol(p, &end, 10);
        p = end;
        if (i < 2 && *p == ':' && isdigit((unsigned char)p[1])) p++;
        else break;
    }
    *out = sign * (parts[0] * 3600 + parts[1] * 60 + parts[2]);
    return p;
}

static const char *parse_change(const char *p, tz_change_t *c) {
    char *end;
    if (*p == 'M') {
        c->kind = 'M';
        c->m = (int)strtol(p + 1, &end, 10);
        if (*end != '.') return NULL;
        c->w = (int)strtol(end + 1, &end, 10);
        if (*end != '.') return NULL;
        c->d = (int)strtol(end + 1, &end, 10);
        if (c->m < 1 || c->m > 12 || c->w < 1 || c->w > 5 || c->d < 0 || c->d > 6) return NULL;
    } else if (*p == 'J') {
        c->kind = 'J';
        c->d = (int)strtol(p + 1, &end, 10);
        if (end == p + 1 || c->d < 1 || c->d > 365) return NULL;
    } else if (isdigit((unsigned char)*p)) {
        c->kind = 'D';
        c->d = (int)strtol(p, &end, 10);
        if (c->d > 365) return NULL;
    } else {
        return NULL;
    }
    p = end;
    c->time = 2 * 3600;
    if (*p == '/') {
        p = parse_hms(p + 1, &c->time);
        if (!p) return NULL;
    }
    return p;
}

bool tz_parse(const char *posix, tz_rules_t *out) {
    memset(out, 0, sizeof *out);
    if (!posix) return false;
    const char *p = parse_name(posix);
    if (!p || !(p = parse_hms(p, &out->std_offset))) return false;
    out->std_offset = -out->std_offset;  // POSIX counts hours west of UTC
    if (!*p) return true;
    if (!(p = parse_name(p))) return false;
    out->has_dst = true;
    out->dst_offset = out->std_offset + 3600;
    if (*p && *p != ',') {
        if (!(p = parse_hms(p, &out->dst_offset))) return false;
        out->dst_offset = -out->dst_offset;
    }
    if (!*p) {  // no rule given: the US rule, as glibc and newlib assume
        out->start = (tz_change_t){'M', 3, 2, 0, 7200};
        out->end = (tz_change_t){'M', 11, 1, 0, 7200};
        return true;
    }
    if (*p != ',' || !(p = parse_change(p + 1, &out->start))) return false;
    if (*p != ',' || !(p = parse_change(p + 1, &out->end))) return false;
    return *p == '\0';
}

static int64_t change_day(const tz_change_t *c, int y) {  // days since epoch of the change date
    int64_t jan1 = days_from_civil(y, 1, 1);
    if (c->kind == 'J') return jan1 + c->d - 1 + (is_leap(y) && c->d >= 60);
    if (c->kind == 'D') return jan1 + c->d;
    int64_t first = days_from_civil(y, c->m, 1);
    int wd_first = (int)((first % 7 + 7 + 4) % 7);  // 1970-01-01 was a Thursday
    int64_t day = first + (c->d - wd_first + 7) % 7 + 7 * (c->w - 1);
    int next_month_y = c->m == 12 ? y + 1 : y, next_m = c->m == 12 ? 1 : c->m + 1;
    while (day >= days_from_civil(next_month_y, next_m, 1)) day -= 7;  // week 5 = last
    return day;
}

static bool in_dst(const tz_rules_t *tz, int64_t utc, int y) {
    int64_t start = change_day(&tz->start, y) * 86400 + tz->start.time - tz->std_offset;
    int64_t end = change_day(&tz->end, y) * 86400 + tz->end.time - tz->dst_offset;
    return start < end ? utc >= start && utc < end : !(utc >= end && utc < start);
}

int tz_rules_offset(const tz_rules_t *tz, int64_t utc) {
    if (!tz->has_dst) return tz->std_offset;
    int y = year_of_days(floor_div(utc + tz->std_offset, 86400));
    return in_dst(tz, utc, y) ? tz->dst_offset : tz->std_offset;
}

int64_t tz_rules_local_to_utc(const tz_rules_t *tz, int y, int mo, int d, int hh, int mi, int ss) {
    int64_t local = days_from_civil(y, mo, d) * 86400 + hh * 3600 + mi * 60 + ss;
    if (!tz->has_dst) return local - tz->std_offset;
    int lo = tz->std_offset < tz->dst_offset ? tz->std_offset : tz->dst_offset;
    int hi = tz->std_offset < tz->dst_offset ? tz->dst_offset : tz->std_offset;
    bool with_hi = tz_rules_offset(tz, local - hi) == hi;  // earlier instant
    bool with_lo = tz_rules_offset(tz, local - lo) == lo;
    if (with_hi) return local - hi;  // also the first of two (clocks went back)
    if (with_lo) return local - lo;
    return local - lo;  // skipped (clocks went forward): the offset before the change
}

int tz_utc_offset(const char *posix, int64_t utc) {
    tz_rules_t tz;
    return tz_parse(posix, &tz) ? tz_rules_offset(&tz, utc) : 0;
}

int64_t tz_local_to_utc(const char *posix, int y, int mo, int d, int hh, int mi, int ss) {
    tz_rules_t tz;
    if (!tz_parse(posix, &tz)) tz_parse("UTC0", &tz);
    return tz_rules_local_to_utc(&tz, y, mo, d, hh, mi, ss);
}
