#include "demo.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// The made-up family (Google event colors: Peacock, Tomato, Banana, Grape).
static const hp_member_t MEMBERS[] = {{"Alex", "7"}, {"Sam", "11"}, {"Jordan", "5"}, {"Riley", "3"}};

static const struct {
    const char *id, *name, *color;
} CALENDARS[DEMO_CALENDARS] = {{"demo00school", "School", "pistachio"}, {"demo00soccer", "Soccer club", "pumpkin"}};

static cJSON *s_family;  // the made-up Family calendar (edits change it)
static int s_next_id;
static SemaphoreHandle_t s_lock;

static void lock(bool take) {
    static StaticSemaphore_t storage;
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&storage);
    if (take) xSemaphoreTake(s_lock, portMAX_DELAY);
    else xSemaphoreGive(s_lock);
}

void demo_members(hp_config_t *cfg) {
    cfg->member_count = sizeof MEMBERS / sizeof MEMBERS[0];
    for (int i = 0; i < cfg->member_count; i++) cfg->members[i] = MEMBERS[i];
}

void demo_calendars(hp_calendars_t *out) {
    memset(out, 0, sizeof *out);
    for (int i = 0; i < DEMO_CALENDARS; i++) {
        hp_calendar_t *c = &out->items[out->count++];
        snprintf(c->id, sizeof c->id, "%s", CALENDARS[i].id);
        snprintf(c->name, sizeof c->name, "%s", CALENDARS[i].name);
        snprintf(c->color, sizeof c->color, "%s", CALENDARS[i].color);
        c->google = true;
    }
}

// --- building events -----------------------------------------------------------------------------

static hp_date_t week_day(int offset) { return hp_date_add(hp_week_start(hp_local_date(time(NULL))), offset); }

static cJSON *utc_time(hp_date_t d, int minutes) {
    time_t t = (time_t)(hp_local_midnight(d) + minutes * 60);
    struct tm tm;
    gmtime_r(&t, &tm);
    char buf[64];
    snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:00Z", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour,
             tm.tm_min);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "dateTime", buf);
    return o;
}

static cJSON *date_time(hp_date_t d) {
    char buf[11];
    hp_format_date(d, buf);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "date", buf);
    return o;
}

static cJSON *item(cJSON *list, const char *title, const char *color_id, const char *location) {
    cJSON *ev = cJSON_CreateObject();
    char id[16];
    snprintf(id, sizeof id, "demo%d", s_next_id++);
    cJSON_AddStringToObject(ev, "id", id);
    cJSON_AddStringToObject(ev, "summary", title);
    if (color_id) cJSON_AddStringToObject(ev, "colorId", color_id);
    if (location) cJSON_AddStringToObject(ev, "location", location);
    cJSON_AddItemToArray(list, ev);
    return ev;
}

// day: days after this week's Sunday; times in minutes after midnight.
static void timed(cJSON *list, int day, int start, int end, const char *title, const char *color_id, const char *location) {
    cJSON *ev = item(list, title, color_id, location);
    cJSON_AddItemToObject(ev, "start", utc_time(week_day(day), start));
    cJSON_AddItemToObject(ev, "end", utc_time(week_day(day), end));
}

static void all_day(cJSON *list, int day, int days, const char *title, const char *color_id) {
    cJSON *ev = item(list, title, color_id, NULL);
    cJSON_AddItemToObject(ev, "start", date_time(week_day(day)));
    cJSON_AddItemToObject(ev, "end", date_time(week_day(day + days)));
}

#define H(h, m) ((h) * 60 + (m))

static cJSON *build_family(void) {
    cJSON *l = cJSON_CreateArray();
    // Last week
    timed(l, -5, H(16, 0), H(17, 0), "Piano lesson", "7", NULL);
    timed(l, -1, H(9, 0), H(10, 0), "Farmers market", NULL, NULL);
    // This week
    timed(l, 0, H(9, 0), H(10, 0), "Pancake breakfast", NULL, NULL);
    timed(l, 0, H(14, 0), H(15, 30), "Bike ride", "3", "Lakeside trail");
    timed(l, 1, H(15, 30), H(16, 15), "Dentist", "11", "Main St Dental");
    timed(l, 1, H(17, 0), H(17, 45), "Swim lesson", "5", NULL);
    timed(l, 2, H(16, 0), H(17, 0), "Piano lesson", "7", NULL);
    timed(l, 2, H(19, 30), H(21, 0), "Book club", "11", NULL);
    all_day(l, 3, 3, "Grandma visiting", NULL);
    timed(l, 3, H(18, 0), H(19, 0), "Science fair", "3", NULL);
    timed(l, 4, H(16, 0), H(16, 30), "Haircut", "5", NULL);
    timed(l, 5, H(18, 30), H(21, 0), "Pizza and movie night", NULL, NULL);
    timed(l, 6, H(9, 0), H(10, 30), "Farmers market", NULL, NULL);
    timed(l, 6, H(14, 0), H(16, 0), "Birthday party", "5", "Party Palace");
    // Next week
    timed(l, 7, H(8, 0), H(9, 0), "Yoga", "11", NULL);
    timed(l, 9, H(16, 0), H(17, 0), "Piano lesson", "7", NULL);
    timed(l, 10, H(19, 0), H(22, 0), "Date night", NULL, NULL);
    timed(l, 11, H(18, 0), H(18, 30), "Parent-teacher conference", "7", NULL);
    all_day(l, 13, 2, "Camping trip", "3");
    return l;
}

cJSON *demo_family_items(void) {
    lock(true);
    if (!s_family) s_family = build_family();
    cJSON *copy = cJSON_Duplicate(s_family, true);
    lock(false);
    return copy;
}

cJSON *demo_calendar_items(int i) {
    cJSON *l = cJSON_CreateArray();
    lock(true);
    if (i == 0) {  // School
        all_day(l, 1, 1, "Picture day", NULL);
        all_day(l, 3, 1, "Early release", NULL);
        timed(l, 4, H(8, 0), H(15, 0), "Book fair", NULL, "Library");
        all_day(l, 8, 1, "Field trip", NULL);
        all_day(l, 12, 1, "No school", NULL);
    } else {  // Soccer club
        for (int week = 0; week <= 7; week += 7) {
            timed(l, week + 2, H(17, 30), H(18, 30), "Practice", NULL, "Riverside Park");
            timed(l, week + 4, H(17, 30), H(18, 30), "Practice", NULL, "Riverside Park");
            timed(l, week + 6, H(11, 0), H(12, 0), week ? "Game vs. Comets" : "Game vs. Rovers", NULL, "Riverside Park");
        }
    }
    lock(false);
    return l;
}

// --- edits (in memory) ------------------------------------------------------------------------------

// Times from the add/edit form come as local "YYYY-MM-DDTHH:MM:SS" + timeZone: store them in UTC.
static void normalize_time(cJSON *ev, const char *key) {
    cJSON *o = cJSON_GetObjectItemCaseSensitive(ev, key);
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(o, "dateTime"));
    int y, mo, d, h, mi, n = 0;
    if (!s || sscanf(s, "%4d-%2d-%2dT%2d:%2d:%*2d%n", &y, &mo, &d, &h, &mi, &n) != 5 || s[n]) return;
    cJSON_ReplaceItemInObjectCaseSensitive(ev, key, utc_time((hp_date_t){y, mo, d}, h * 60 + mi));
}

static void normalize(cJSON *ev) {
    normalize_time(ev, "start");
    normalize_time(ev, "end");
}

static cJSON *find(const char *id) {  // lock held
    cJSON *ev;
    cJSON_ArrayForEach(ev, s_family) {
        const char *x = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(ev, "id"));
        if (x && id && strcmp(x, id) == 0) return ev;
    }
    return NULL;
}

static hp_edit_result_t op_get(void *ctx, const char *id, cJSON **out) {
    (void)ctx;
    lock(true);
    cJSON *ev = find(id);
    if (ev && out) *out = cJSON_Duplicate(ev, true);
    lock(false);
    return ev ? HP_EDIT_OK : HP_EDIT_NOT_FOUND;
}

static hp_edit_result_t op_insert(void *ctx, const cJSON *body, cJSON **out) {
    (void)ctx;
    cJSON *ev = cJSON_Duplicate(body, true);
    if (!ev) return HP_EDIT_FAILED;
    normalize(ev);
    lock(true);
    if (!s_family) s_family = build_family();
    char id[16];
    snprintf(id, sizeof id, "demo%d", s_next_id++);
    cJSON_DeleteItemFromObjectCaseSensitive(ev, "id");
    cJSON_AddStringToObject(ev, "id", id);
    cJSON_AddItemToArray(s_family, ev);
    if (out) *out = cJSON_Duplicate(ev, true);
    lock(false);
    return HP_EDIT_OK;
}

static hp_edit_result_t change(const char *id, const cJSON *body, bool whole, cJSON **out) {
    lock(true);
    cJSON *ev = find(id);
    if (!ev) {
        lock(false);
        return HP_EDIT_NOT_FOUND;
    }
    if (whole) {  // update: the body replaces everything but the id
        cJSON *keep = cJSON_DetachItemFromObjectCaseSensitive(ev, "id");
        while (ev->child) cJSON_Delete(cJSON_DetachItemViaPointer(ev, ev->child));
        cJSON_AddItemToObject(ev, "id", keep);
    }
    const cJSON *field;
    cJSON_ArrayForEach(field, body) {
        if (strcmp(field->string, "id") == 0) continue;
        cJSON_DeleteItemFromObjectCaseSensitive(ev, field->string);
        cJSON_AddItemToObject(ev, field->string, cJSON_Duplicate(field, true));
    }
    normalize(ev);
    if (out) *out = cJSON_Duplicate(ev, true);
    lock(false);
    return HP_EDIT_OK;
}

static hp_edit_result_t op_update(void *ctx, const char *id, const cJSON *body, cJSON **out) {
    (void)ctx;
    return change(id, body, true, out);
}

static hp_edit_result_t op_patch(void *ctx, const char *id, const cJSON *body, cJSON **out) {
    (void)ctx;
    return change(id, body, false, out);
}

static hp_edit_result_t op_remove(void *ctx, const char *id) {
    (void)ctx;
    lock(true);
    cJSON *ev = find(id);
    if (ev) cJSON_Delete(cJSON_DetachItemViaPointer(s_family, ev));
    lock(false);
    return ev ? HP_EDIT_OK : HP_EDIT_NOT_FOUND;
}

hp_calendar_ops_t demo_ops(void) { return (hp_calendar_ops_t){NULL, op_get, op_insert, op_update, op_patch, op_remove}; }

// --- weather ----------------------------------------------------------------------------------------

void demo_weather(bool fahrenheit, weather_report_t *out) {
    static const int CODES[WEATHER_MAX_DAYS] = {1, 2, 61, 3, 80, 0, 1, 95, 45, 2, 0, 63, 3, 1, 2, 0};
    memset(out, 0, sizeof *out);
    out->valid = true;
    out->unit = fahrenheit ? 'F' : 'C';
    out->code = 1;
    float c = 18;
    out->temperature = fahrenheit ? c * 9 / 5 + 32 : c;
    hp_date_t today = hp_local_date(time(NULL));
    for (int i = 0; i < WEATHER_MAX_DAYS; i++) {
        weather_day_t *d = &out->days[i];
        hp_date_t date = hp_date_add(today, i);
        d->y = date.y, d->m = date.m, d->d = date.d;
        float high = 19 + i % 4, low = 9 + i % 3;
        d->high = fahrenheit ? high * 9 / 5 + 32 : high;
        d->low = fahrenheit ? low * 9 / 5 + 32 : low;
        d->precip_chance = CODES[i] >= 51 ? 60 + (i * 7) % 30 : (i * 13) % 20;
        d->code = CODES[i];
    }
    out->day_count = WEATHER_MAX_DAYS;
}
