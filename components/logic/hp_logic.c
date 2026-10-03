// HomePlanner calendar logic. See hp_logic.h. Mirrors the Raspberry Pi version's events.py.
#include "hp_logic.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

// --- small string helpers ------------------------------------------------------------------

static void copy_str(char *dst, size_t size, const char *src) {
    if (size == 0) return;
    snprintf(dst, size, "%s", src ? src : "");
}

// Copy src without leading/trailing whitespace.
static void copy_trimmed(char *dst, size_t size, const char *src) {
    if (!src) src = "";
    while (isspace((unsigned char)*src)) src++;
    size_t n = strlen(src);
    while (n > 0 && isspace((unsigned char)src[n - 1])) n--;
    if (n >= size) n = size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool trimmed_equal(const char *a, const char *b) {
    char ta[HP_TEXT_LEN], tb[HP_TEXT_LEN];
    copy_trimmed(ta, sizeof ta, a);
    copy_trimmed(tb, sizeof tb, b);
    return strcmp(ta, tb) == 0;
}

static const char *json_str(const cJSON *obj, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(obj, key);
    return cJSON_IsString(item) ? item->valuestring : NULL;
}

// --- colors --------------------------------------------------------------------------------

static const struct {
    const char *id, *hex, *name;
} COLORS[] = {
    {"1", "#7986cb", "Lavender"}, {"2", "#33b679", "Sage"},      {"3", "#8e24aa", "Grape"},
    {"4", "#e67c73", "Flamingo"}, {"5", "#f6bf26", "Banana"},    {"6", "#f4511e", "Tangerine"},
    {"7", "#039be5", "Peacock"},  {"8", "#616161", "Graphite"},  {"9", "#3f51b5", "Blueberry"},
    {"10", "#0b8043", "Basil"},   {"11", "#d50000", "Tomato"},
};

const char *hp_color_hex(const char *id) {
    for (size_t i = 0; id && i < sizeof COLORS / sizeof COLORS[0]; i++)
        if (strcmp(COLORS[i].id, id) == 0) return COLORS[i].hex;
    return NULL;
}

const char *hp_color_name(const char *id) {
    for (size_t i = 0; id && i < sizeof COLORS / sizeof COLORS[0]; i++)
        if (strcmp(COLORS[i].id, id) == 0) return COLORS[i].name;
    return NULL;
}

// Google's 24-color calendar palette in its picker order (rows of six): the 11 event colors
// plus 13 more, keyed by name (as in the Pi version's colors.py).
const hp_palette_color_t HP_PALETTE[HP_PALETTE_COUNT] = {
    {"radicchio", "Radicchio", "#ad1457"}, {"6", "Tangerine", "#f4511e"},
    {"citron", "Citron", "#e4c441"},       {"10", "Basil", "#0b8043"},
    {"9", "Blueberry", "#3f51b5"},          {"3", "Grape", "#8e24aa"},
    {"cherry-blossom", "Cherry blossom", "#d81b60"}, {"pumpkin", "Pumpkin", "#ef6c00"},
    {"avocado", "Avocado", "#c0ca33"},     {"eucalyptus", "Eucalyptus", "#009688"},
    {"1", "Lavender", "#7986cb"},           {"cocoa", "Cocoa", "#795548"},
    {"11", "Tomato", "#d50000"},            {"mango", "Mango", "#f09300"},
    {"pistachio", "Pistachio", "#7cb342"}, {"7", "Peacock", "#039be5"},
    {"wisteria", "Wisteria", "#b39ddb"},   {"8", "Graphite", "#616161"},
    {"4", "Flamingo", "#e67c73"},           {"5", "Banana", "#f6bf26"},
    {"2", "Sage", "#33b679"},               {"cobalt", "Cobalt", "#4285f4"},
    {"amethyst", "Amethyst", "#9e69af"},   {"birch", "Birch", "#a79b8e"},
};

const char *hp_palette_hex(const char *id) {
    for (int i = 0; id && i < HP_PALETTE_COUNT; i++)
        if (strcmp(HP_PALETTE[i].id, id) == 0) return HP_PALETTE[i].hex;
    return NULL;
}

const char *hp_member_for_color(const hp_config_t *cfg, const char *color_id) {
    for (int i = 0; color_id && i < cfg->member_count; i++)
        if (strcmp(cfg->members[i].color_id, color_id) == 0) return cfg->members[i].name;
    return NULL;
}

const char *hp_color_for_member(const hp_config_t *cfg, const char *member) {
    for (int i = 0; member && i < cfg->member_count; i++)
        if (strcmp(cfg->members[i].name, member) == 0) return cfg->members[i].color_id;
    return NULL;
}

// --- dates ---------------------------------------------------------------------------------
// Civil-date arithmetic after Howard Hinnant's algorithms (proleptic Gregorian).

int64_t hp_days_from_civil(hp_date_t d) {
    int y = d.y - (d.m <= 2);
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (153 * (unsigned)(d.m + (d.m > 2 ? -3 : 9)) + 2) / 5 + (unsigned)d.d - 1;
    unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}

hp_date_t hp_civil_from_days(int64_t z) {
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t y = (int64_t)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    unsigned mp = (5 * doy + 2) / 153;
    hp_date_t out;
    out.d = (int)(doy - (153 * mp + 2) / 5 + 1);
    out.m = (int)(mp < 10 ? mp + 3 : mp - 9);
    out.y = (int)(y + (out.m <= 2));
    return out;
}

hp_date_t hp_date_add(hp_date_t d, int days) { return hp_civil_from_days(hp_days_from_civil(d) + days); }

int hp_date_cmp(hp_date_t a, hp_date_t b) {
    int64_t da = hp_days_from_civil(a), db = hp_days_from_civil(b);
    return da < db ? -1 : da > db ? 1 : 0;
}

int hp_date_diff(hp_date_t a, hp_date_t b) { return (int)(hp_days_from_civil(a) - hp_days_from_civil(b)); }

int hp_weekday(hp_date_t d) {
    int64_t w = (hp_days_from_civil(d) + 4) % 7;  // 1970-01-01 was a Thursday
    return (int)(w < 0 ? w + 7 : w);
}

hp_date_t hp_week_start(hp_date_t d) { return hp_date_add(d, -hp_weekday(d)); }

bool hp_parse_date(const char *s, hp_date_t *out) {
    int y, m, d;
    if (!s || sscanf(s, "%4d-%2d-%2d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return false;
    out->y = y, out->m = m, out->d = d;
    return true;
}

void hp_format_date(hp_date_t d, char out[11]) { snprintf(out, 11, "%04d-%02d-%02d", d.y, d.m, d.d); }

int64_t hp_local_midnight(hp_date_t d) {
    struct tm tm = {0};
    tm.tm_year = d.y - 1900;
    tm.tm_mon = d.m - 1;
    tm.tm_mday = d.d;
    tm.tm_isdst = -1;
    return (int64_t)mktime(&tm);
}

hp_date_t hp_local_date(int64_t t) {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    return (hp_date_t){tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday};
}

static int local_minutes(int64_t t) {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    return tm.tm_hour * 60 + tm.tm_min;
}

bool hp_parse_rfc3339(const char *s, int64_t *out) {
    int y, mo, d, h, mi, sec, n = 0;
    if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%2d%n", &y, &mo, &d, &h, &mi, &sec, &n) != 6) return false;
    const char *p = s + n;
    if (*p == '.')  // fractional seconds: ignore
        for (p++; isdigit((unsigned char)*p); p++) {}
    int offset = 0;
    if (*p == 'Z' || *p == 'z') {
        offset = 0;
    } else if (*p == '+' || *p == '-') {
        int oh, om;
        if (sscanf(p + 1, "%2d:%2d", &oh, &om) != 2) return false;
        offset = (oh * 60 + om) * 60 * (*p == '-' ? -1 : 1);
    } else {
        return false;  // no zone: Google always sends one
    }
    *out = hp_days_from_civil((hp_date_t){y, mo, d}) * 86400 + h * 3600 + mi * 60 + sec - offset;
    return true;
}

// Google start/end object -> instant (+ whether it's an all-day date).
static bool parse_google_time(const cJSON *obj, int64_t *t, bool *all_day) {
    const char *date = json_str(obj, "date");
    if (date) {
        hp_date_t d;
        if (!hp_parse_date(date, &d)) return false;
        *t = hp_local_midnight(d);
        *all_day = true;
        return true;
    }
    *all_day = false;
    return hp_parse_rfc3339(json_str(obj, "dateTime"), t);
}

// --- HTML notes -> plain text ------------------------------------------------------------------

typedef struct {
    char *out;
    size_t size, len;
    int newlines;  // trailing newlines already written
} text_buf_t;

static void put_char(text_buf_t *b, char c) {
    if (b->len + 1 < b->size) b->out[b->len++] = c;
}

static void put_text_char(text_buf_t *b, char c) {
    if (b->len == 0 && (c == ' ' || c == '\t')) return;  // no leading spaces
    put_char(b, c);
    b->newlines = 0;
}

static void hard_newline(text_buf_t *b) {
    if (b->len == 0 || b->newlines >= 2) return;  // at most one blank line
    put_char(b, '\n');
    b->newlines++;
}

static void soft_break(text_buf_t *b) {
    if (b->len > 0 && b->newlines == 0) hard_newline(b);
}

static void put_utf8(text_buf_t *b, unsigned long cp) {
    if (cp == 0xA0) cp = ' ';  // non-breaking space
    if (cp < 0x80) put_text_char(b, (char)cp);
    else if (cp < 0x800) put_text_char(b, (char)(0xC0 | (cp >> 6))), put_text_char(b, (char)(0x80 | (cp & 0x3F)));
    else if (cp < 0x10000)
        put_text_char(b, (char)(0xE0 | (cp >> 12))), put_text_char(b, (char)(0x80 | ((cp >> 6) & 0x3F))),
            put_text_char(b, (char)(0x80 | (cp & 0x3F)));
}

static bool tag_is(const char *name, const char *const *list) {
    for (; *list; list++)
        if (strcmp(name, *list) == 0) return true;
    return false;
}

void hp_html_to_text(const char *html, char *out, size_t size) {
    static const char *const BLOCK[] = {"div", "blockquote", "tr", "ul", "ol", "h1", "h2", "h3", "h4", "h5", "h6", NULL};
    text_buf_t b = {out, size, 0, 0};
    if (size == 0) return;
    for (const char *p = html ? html : ""; *p;) {
        if (*p == '<' && (isalpha((unsigned char)p[1]) || p[1] == '/' || p[1] == '!')) {
            const char *end = strchr(p, '>');
            if (!end) {  // not a tag after all
                put_text_char(&b, *p++);
                continue;
            }
            const char *q = p + 1;
            bool closing = *q == '/';
            if (closing) q++;
            char name[12] = "";
            for (size_t n = 0; isalnum((unsigned char)*q) && n + 1 < sizeof name; q++) {
                name[n++] = (char)tolower((unsigned char)*q);
                name[n] = '\0';
            }
            if (strcmp(name, "br") == 0) hard_newline(&b);
            else if (strcmp(name, "p") == 0 && closing) soft_break(&b), hard_newline(&b);
            else if (strcmp(name, "li") == 0) {
                soft_break(&b);
                if (!closing) put_text_char(&b, '-'), put_text_char(&b, ' ');
            } else if (tag_is(name, BLOCK)) soft_break(&b);
            p = end + 1;
        } else if (*p == '&') {
            const char *semi = strchr(p, ';');
            unsigned long cp = 0;
            bool ok = semi && semi - p <= 8;
            if (ok) {
                size_t n = (size_t)(semi - p - 1);
                const char *e = p + 1;
                if (n == 3 && !strncmp(e, "amp", 3)) cp = '&';
                else if (n == 2 && !strncmp(e, "lt", 2)) cp = '<';
                else if (n == 2 && !strncmp(e, "gt", 2)) cp = '>';
                else if (n == 4 && !strncmp(e, "quot", 4)) cp = '"';
                else if (n == 4 && !strncmp(e, "apos", 4)) cp = '\'';
                else if (n == 4 && !strncmp(e, "nbsp", 4)) cp = ' ';
                else if (*e == '#') cp = (e[1] == 'x' || e[1] == 'X') ? strtoul(e + 2, NULL, 16) : strtoul(e + 1, NULL, 10);
                else ok = false;
            }
            if (ok && cp) {
                put_utf8(&b, cp);
                p = semi + 1;
            } else {
                put_text_char(&b, *p++);
            }
        } else if (*p == '\r') {
            p++;
        } else if (*p == '\n') {
            hard_newline(&b);
            p++;
        } else {
            put_text_char(&b, *p++);
        }
    }
    while (b.len > 0 && isspace((unsigned char)out[b.len - 1])) b.len--;  // no trailing blank lines
    out[b.len] = '\0';
}

// --- events --------------------------------------------------------------------------------

bool hp_is_owned(const cJSON *ev, const hp_config_t *cfg) {
    const cJSON *org = cJSON_GetObjectItemCaseSensitive(ev, "organizer");
    if (!cJSON_IsObject(org)) return true;
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(org, "self"))) return true;
    const char *email = json_str(org, "email");
    return email && strcmp(email, cfg->calendar_id) == 0;
}

bool hp_event_from_google(const cJSON *ev, const hp_config_t *cfg, hp_event_t *out) {
    memset(out, 0, sizeof *out);
    const char *status = json_str(ev, "status");
    if (status && strcmp(status, "cancelled") == 0) return false;
    bool end_all_day;
    if (!parse_google_time(cJSON_GetObjectItemCaseSensitive(ev, "start"), &out->start, &out->all_day) ||
        !parse_google_time(cJSON_GetObjectItemCaseSensitive(ev, "end"), &out->end, &end_all_day))
        return false;
    copy_str(out->id, sizeof out->id, json_str(ev, "id"));
    const char *summary = json_str(ev, "summary");
    copy_str(out->title, sizeof out->title, summary && *summary ? summary : "(No title)");
    copy_str(out->location, sizeof out->location, json_str(ev, "location"));
    hp_html_to_text(json_str(ev, "description"), out->description, sizeof out->description);
    const char *color_id = json_str(ev, "colorId");
    copy_str(out->member, sizeof out->member, hp_member_for_color(cfg, color_id));
    const char *hex = hp_color_hex(color_id);
    copy_str(out->color, sizeof out->color, hex ? hex : HP_FAMILY_COLOR);
    out->editable = true;
    const char *series = json_str(ev, "recurringEventId");
    out->recurring = series != NULL || cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(ev, "recurrence"));
    copy_str(out->series_id, sizeof out->series_id, series);
    out->owned = hp_is_owned(ev, cfg);
    return true;
}

bool hp_overlaps_day(const hp_event_t *e, hp_date_t d) {
    int64_t day_start = hp_local_midnight(d), day_end = hp_local_midnight(hp_date_add(d, 1));
    return e->start < day_end && e->end > day_start;
}

// --- form ----------------------------------------------------------------------------------

const char *hp_form_validate(const hp_form_t *f) {
    char title[HP_TITLE_LEN];
    copy_trimmed(title, sizeof title, f->title);
    if (!title[0]) return "Please enter a title.";
    if (!f->all_day) {
        if (f->start_min < 0 || f->end_min < 0) return "Please enter start and end times, or choose All day.";
        if (f->end_min <= f->start_min) return "End time must be after the start time.";
    }
    if (f->has_until) {
        if (f->repeat == HP_REPEAT_NONE) return "Choose how it repeats before setting an end date.";
        if (hp_date_cmp(f->until, f->date) < 0) return "Repeat-until must be on or after the date.";
    }
    return NULL;
}

static const char *FREQ_NAMES[] = {"", "DAILY", "WEEKLY", "MONTHLY", "YEARLY"};

static void rrule_for(const hp_form_t *f, char *out, size_t size) {
    int n = snprintf(out, size, "RRULE:FREQ=%s", FREQ_NAMES[f->repeat]);
    if (!f->has_until) return;
    if (f->all_day) {
        snprintf(out + n, size - (size_t)n, ";UNTIL=%04d%02d%02d", f->until.y, f->until.m, f->until.d);
    } else {
        // RFC 5545: with a zoned DTSTART, UNTIL must be UTC. Use the end of the local day.
        time_t t = (time_t)(hp_local_midnight(hp_date_add(f->until, 1)) - 1);
        struct tm tm;
        gmtime_r(&t, &tm);
        snprintf(out + n, size - (size_t)n, ";UNTIL=%04d%02d%02dT%02d%02d%02dZ", tm.tm_year + 1900,
                 tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    }
}

static cJSON *date_obj(hp_date_t d) {
    char buf[11];
    hp_format_date(d, buf);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "date", buf);
    return o;
}

static cJSON *datetime_obj(hp_date_t d, int minutes, const char *tz) {
    char buf[32];
    snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:00", d.y, d.m, d.d, minutes / 60, minutes % 60);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "dateTime", buf);
    cJSON_AddStringToObject(o, "timeZone", tz);
    return o;
}

cJSON *hp_form_to_google(const hp_form_t *f, const hp_config_t *cfg, const char **err) {
    char buf[HP_TEXT_LEN];
    const char *color = NULL;
    if (f->member[0]) {
        color = hp_color_for_member(cfg, f->member);
        if (!color) {
            if (err) *err = "Unknown family member.";
            return NULL;
        }
    }
    cJSON *p = cJSON_CreateObject();
    copy_trimmed(buf, sizeof buf, f->title);
    cJSON_AddStringToObject(p, "summary", buf);
    if (f->all_day) {
        cJSON_AddItemToObject(p, "start", date_obj(f->date));
        cJSON_AddItemToObject(p, "end", date_obj(hp_date_add(f->date, 1)));
    } else {
        cJSON_AddItemToObject(p, "start", datetime_obj(f->date, f->start_min, cfg->timezone));
        cJSON_AddItemToObject(p, "end", datetime_obj(f->date, f->end_min, cfg->timezone));
    }
    if (color) cJSON_AddStringToObject(p, "colorId", color);
    copy_trimmed(buf, sizeof buf, f->location);
    if (buf[0]) cJSON_AddStringToObject(p, "location", buf);
    copy_trimmed(buf, sizeof buf, f->description);
    if (buf[0]) cJSON_AddStringToObject(p, "description", buf);
    if (f->repeat != HP_REPEAT_NONE) {
        char rule[96];
        rrule_for(f, rule, sizeof rule);
        cJSON *rec = cJSON_CreateArray();
        cJSON_AddItemToArray(rec, cJSON_CreateString(rule));
        cJSON_AddItemToObject(p, "recurrence", rec);
    }
    return p;
}

// --- repeat rules ----------------------------------------------------------------------------

static const char *WEEKDAY_CODES[] = {"SU", "MO", "TU", "WE", "TH", "FR", "SA"};

static bool parse_until(const char *v, hp_date_t *out) {
    int y, m, d, H, M, S;
    size_t len = strlen(v);
    if (len >= 16 && (v[len - 1] == 'Z' || v[len - 1] == 'z') &&
        sscanf(v, "%4d%2d%2dT%2d%2d%2d", &y, &m, &d, &H, &M, &S) == 6) {
        int64_t t = hp_days_from_civil((hp_date_t){y, m, d}) * 86400 + H * 3600 + M * 60 + S;
        *out = hp_local_date(t);
        return true;
    }
    if (sscanf(v, "%4d%2d%2d", &y, &m, &d) == 3) {
        *out = (hp_date_t){y, m, d};
        return true;
    }
    return false;
}

bool hp_parse_simple_rrule(const cJSON *recurrence, hp_date_t start, hp_repeat_t *repeat,
                           bool *has_until, hp_date_t *until) {
    *repeat = HP_REPEAT_NONE;
    *has_until = false;
    const char *rule = NULL;
    int rules = 0;
    const cJSON *line;
    cJSON_ArrayForEach(line, recurrence) {
        if (cJSON_IsString(line) && strncasecmp(line->valuestring, "RRULE:", 6) == 0) {
            rule = line->valuestring + 6;
            rules++;
        }
    }
    if (rules == 0) return true;
    if (rules > 1) return false;

    char upper[256], copy[256];
    copy_str(upper, sizeof upper, rule);
    for (char *c = upper; *c; c++) *c = (char)toupper((unsigned char)*c);
    memcpy(copy, upper, sizeof copy);  // strtok_r below modifies copy

    hp_repeat_t freq = HP_REPEAT_NONE;
    bool simple = true;
    char *save = NULL;
    for (char *part = strtok_r(copy, ";", &save); part; part = strtok_r(NULL, ";", &save)) {
        char *eq = strchr(part, '=');
        if (!eq) continue;
        *eq = '\0';
        const char *key = part, *val = eq + 1;
        if (strcmp(key, "FREQ") == 0) {
            for (int i = HP_REPEAT_DAILY; i <= HP_REPEAT_YEARLY; i++)
                if (strcmp(val, FREQ_NAMES[i]) == 0) freq = (hp_repeat_t)i;
        } else if (strcmp(key, "WKST") == 0) {
            // ignored
        } else if (strcmp(key, "INTERVAL") == 0) {
            if (strcmp(val, "1") != 0) simple = false;
        } else if (strcmp(key, "UNTIL") == 0) {
            if (!parse_until(val, until)) simple = false;
            *has_until = true;
        } else {
            // Rules Google's own app writes for plain repeats count as simple.
            bool plain_weekly = strcmp(key, "BYDAY") == 0 && strcmp(val, WEEKDAY_CODES[hp_weekday(start)]) == 0;
            char day[4];
            snprintf(day, sizeof day, "%d", start.d);
            bool plain_monthly = strcmp(key, "BYMONTHDAY") == 0 && strcmp(val, day) == 0;
            // BYDAY/BYMONTHDAY are only "plain" for the matching frequency; checked after the loop.
            if (plain_weekly) continue;
            if (plain_monthly) continue;
            simple = false;  // COUNT, BYSETPOS, BYDAY lists...
        }
    }
    if (freq == HP_REPEAT_NONE) return false;
    // Re-check BY* parts against the frequency they are plain for.
    if (strstr(upper, "BYDAY") && freq != HP_REPEAT_WEEKLY) simple = false;
    if (strstr(upper, "BYMONTHDAY") && freq != HP_REPEAT_MONTHLY) simple = false;
    if (!simple) {
        *has_until = false;
        return false;
    }
    *repeat = freq;
    return true;
}

int hp_all_day_span(const cJSON *ev) {
    hp_date_t s, e;
    const char *sd = json_str(cJSON_GetObjectItemCaseSensitive(ev, "start"), "date");
    const char *ed = json_str(cJSON_GetObjectItemCaseSensitive(ev, "end"), "date");
    if (!sd || !ed || !hp_parse_date(sd, &s) || !hp_parse_date(ed, &e)) return 1;
    int days = hp_date_diff(e, s);
    return days > 1 ? days : 1;
}

hp_date_t hp_occurrence_date(const cJSON *ev) {
    const cJSON *src = cJSON_GetObjectItemCaseSensitive(ev, "originalStartTime");
    if (!cJSON_IsObject(src)) src = cJSON_GetObjectItemCaseSensitive(ev, "start");
    int64_t t = 0;
    bool all_day = false;
    parse_google_time(src, &t, &all_day);
    return hp_local_date(t);
}

// --- editing existing events ------------------------------------------------------------------

bool hp_event_to_form(const cJSON *ev, const cJSON *series, const hp_config_t *cfg, hp_event_details_t *out) {
    memset(out, 0, sizeof *out);
    int64_t start, end;
    bool all_day, end_all_day;
    if (!parse_google_time(cJSON_GetObjectItemCaseSensitive(ev, "start"), &start, &all_day) ||
        !parse_google_time(cJSON_GetObjectItemCaseSensitive(ev, "end"), &end, &end_all_day))
        return false;
    hp_form_t *f = &out->form;
    copy_str(f->title, sizeof f->title, json_str(ev, "summary"));
    f->date = hp_local_date(start);
    f->all_day = all_day;
    f->start_min = all_day ? -1 : local_minutes(start);
    f->end_min = all_day ? -1 : local_minutes(end);
    copy_str(f->member, sizeof f->member, hp_member_for_color(cfg, json_str(ev, "colorId")));
    copy_str(f->location, sizeof f->location, json_str(ev, "location"));
    hp_html_to_text(json_str(ev, "description"), f->description, sizeof f->description);

    const cJSON *rule_src = series ? series : ev;
    int64_t rule_start = start;
    bool ignored;
    parse_google_time(cJSON_GetObjectItemCaseSensitive(rule_src, "start"), &rule_start, &ignored);
    bool simple = hp_parse_simple_rrule(cJSON_GetObjectItemCaseSensitive(rule_src, "recurrence"),
                                        hp_local_date(rule_start), &f->repeat, &f->has_until, &f->until);
    out->custom_repeat = !simple;
    out->recurring = json_str(ev, "recurringEventId") != NULL ||
                     cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(ev, "recurrence"));
    out->span_days = hp_all_day_span(ev);
    out->owned = hp_is_owned(ev, cfg);
    // The form has one date; events crossing midnight can only be deleted here.
    hp_date_t end_date = hp_local_date(end);
    bool ends_at_next_midnight = end == hp_local_midnight(hp_date_add(f->date, 1));
    out->form_editable = all_day || hp_date_cmp(end_date, f->date) <= 0 || ends_at_next_midnight;
    return true;
}

static const char *FORM_FIELDS[] = {"summary", "start", "end", "colorId", "location", "description"};

cJSON *hp_apply_form(const cJSON *resource, const hp_form_t *f, const hp_config_t *cfg,
                     bool keep_recurrence, int span_days, const char **err) {
    cJSON *new_fields = hp_form_to_google(f, cfg, err);
    if (!new_fields) return NULL;
    if (f->all_day && span_days > 1)
        cJSON_ReplaceItemInObjectCaseSensitive(new_fields, "end", date_obj(hp_date_add(f->date, span_days)));

    cJSON *body = cJSON_Duplicate(resource, true);
    for (size_t i = 0; i < sizeof FORM_FIELDS / sizeof FORM_FIELDS[0]; i++)
        cJSON_DeleteItemFromObjectCaseSensitive(body, FORM_FIELDS[i]);

    if (keep_recurrence) {
        cJSON_DeleteItemFromObjectCaseSensitive(new_fields, "recurrence");
    } else {
        cJSON *new_rec = cJSON_GetObjectItemCaseSensitive(new_fields, "recurrence");
        const cJSON *old_rec = cJSON_GetObjectItemCaseSensitive(resource, "recurrence");
        if (new_rec) {  // keep EXDATE/RDATE lines (deleted or extra occurrences)
            const cJSON *line;
            cJSON_ArrayForEach(line, old_rec) {
                if (cJSON_IsString(line) && strncasecmp(line->valuestring, "RRULE:", 6) != 0)
                    cJSON_AddItemToArray(new_rec, cJSON_CreateString(line->valuestring));
            }
        }
        cJSON_DeleteItemFromObjectCaseSensitive(body, "recurrence");
    }
    // Move every new field into the body.
    while (new_fields->child) {
        cJSON *item = cJSON_DetachItemViaPointer(new_fields, new_fields->child);
        cJSON_AddItemToObject(body, item->string, item);
    }
    cJSON_Delete(new_fields);
    return body;
}

int hp_guest_changes(const hp_form_t *current, const hp_form_t *f) {
    int changed = 0;
    if (!trimmed_equal(f->title, current->title)) changed |= HP_CHG_TITLE;
    if (hp_date_cmp(f->date, current->date) != 0 || f->all_day != current->all_day) changed |= HP_CHG_DATE;
    if (!f->all_day && (f->start_min != current->start_min || f->end_min != current->end_min)) changed |= HP_CHG_TIME;
    if (!trimmed_equal(f->location, current->location)) changed |= HP_CHG_LOCATION;
    if (!trimmed_equal(f->description, current->description)) changed |= HP_CHG_NOTES;
    return changed;
}
