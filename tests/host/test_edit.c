// Host tests for hp_edit (add / edit / delete rules), with an in-memory fake Google calendar.
// Scenarios mirror the Pi version's tests/test_edit_events.py and test_guest_events.py.
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hp_edit.h"
#include "unity.h"

static hp_config_t CFG;

// --- fake calendar ---------------------------------------------------------------------------
typedef struct {
    cJSON *events;  // array
    char last_op[8], last_id[160];
    cJSON *last_body;
    int calls;
    bool offline;
} fake_t;

static cJSON *find(fake_t *f, const char *id) {
    cJSON *e;
    cJSON_ArrayForEach(e, f->events) {
        const cJSON *i = cJSON_GetObjectItem(e, "id");
        if (cJSON_IsString(i) && strcmp(i->valuestring, id) == 0) return e;
    }
    return NULL;
}

static void record(fake_t *f, const char *op, const char *id, const cJSON *body) {
    snprintf(f->last_op, sizeof f->last_op, "%s", op);
    snprintf(f->last_id, sizeof f->last_id, "%s", id ? id : "");
    cJSON_Delete(f->last_body);
    f->last_body = body ? cJSON_Duplicate(body, true) : NULL;
    f->calls++;
}

static hp_edit_result_t f_get(void *ctx, const char *id, cJSON **out) {
    fake_t *f = ctx;
    if (f->offline) return HP_EDIT_NETWORK;
    cJSON *e = find(f, id);
    if (!e) return HP_EDIT_NOT_FOUND;
    *out = cJSON_Duplicate(e, true);
    return HP_EDIT_OK;
}
static hp_edit_result_t f_insert(void *ctx, const cJSON *body, cJSON **out) {
    fake_t *f = ctx;
    if (f->offline) return HP_EDIT_NETWORK;
    record(f, "insert", NULL, body);
    cJSON *e = cJSON_Duplicate(body, true);
    cJSON_AddStringToObject(e, "id", "new-1");
    cJSON_AddItemToArray(f->events, e);
    if (out) *out = cJSON_Duplicate(e, true);
    return HP_EDIT_OK;
}
static hp_edit_result_t f_update(void *ctx, const char *id, const cJSON *body, cJSON **out) {
    fake_t *f = ctx;
    if (!find(f, id)) return HP_EDIT_NOT_FOUND;
    record(f, "update", id, body);
    (void)out;
    return HP_EDIT_OK;
}
static hp_edit_result_t f_patch(void *ctx, const char *id, const cJSON *body, cJSON **out) {
    fake_t *f = ctx;
    if (!find(f, id)) return HP_EDIT_NOT_FOUND;
    record(f, "patch", id, body);
    (void)out;
    return HP_EDIT_OK;
}
static hp_edit_result_t f_remove(void *ctx, const char *id) {
    fake_t *f = ctx;
    if (!find(f, id)) return HP_EDIT_NOT_FOUND;
    record(f, "delete", id, NULL);
    return HP_EDIT_OK;
}

static fake_t F;
static hp_calendar_ops_t OPS = {&F, f_get, f_insert, f_update, f_patch, f_remove};

#define LA "\"timeZone\":\"America/Los_Angeles\""
#define SERIES                                                                                                  \
    "{\"id\":\"soccer\",\"summary\":\"Soccer\",\"colorId\":\"11\",\"location\":\"Park\",\"etag\":\"e1\","          \
    "\"start\":{\"dateTime\":\"2026-10-06T17:00:00-07:00\"," LA "},\"end\":{\"dateTime\":\"2026-10-06T18:30:00-07:00\"," LA "}," \
    "\"recurrence\":[\"RRULE:FREQ=WEEKLY;BYDAY=TU\",\"EXDATE;TZID=America/Los_Angeles:20261020T170000\"]}"
#define OCC1                                                                                                    \
    "{\"id\":\"soccer_1013\",\"summary\":\"Soccer\",\"colorId\":\"11\",\"location\":\"Park\",\"recurringEventId\":\"soccer\"," \
    "\"originalStartTime\":{\"dateTime\":\"2026-10-13T17:00:00-07:00\"},"                                      \
    "\"start\":{\"dateTime\":\"2026-10-13T17:00:00-07:00\"},\"end\":{\"dateTime\":\"2026-10-13T18:30:00-07:00\"}}"
#define DENTIST                                                                                                 \
    "{\"id\":\"dentist\",\"summary\":\"Dentist\",\"colorId\":\"9\",\"description\":\"<div>Bring <b>card</b></div>\"," \
    "\"start\":{\"dateTime\":\"2026-10-07T15:30:00-07:00\"},\"end\":{\"dateTime\":\"2026-10-07T16:15:00-07:00\"}}"
#define PARTY                                                                                                   \
    "{\"id\":\"party\",\"summary\":\"Birthday party\",\"colorId\":\"9\",\"organizer\":{\"email\":\"parent@example.com\"}," \
    "\"start\":{\"dateTime\":\"2026-10-09T15:00:00-07:00\"},\"end\":{\"dateTime\":\"2026-10-09T17:00:00-07:00\"}}"

void setUp(void) {
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);
    tzset();
    memset(&CFG, 0, sizeof CFG);
    strcpy(CFG.calendar_id, "family@group.calendar.google.com");
    strcpy(CFG.timezone, "America/Los_Angeles");
    strcpy(CFG.members[0].name, "Alex"), strcpy(CFG.members[0].color_id, "9");
    strcpy(CFG.members[1].name, "Sam"), strcpy(CFG.members[1].color_id, "11");
    CFG.member_count = 2;
    memset(&F, 0, sizeof F);
    F.events = cJSON_Parse("[" SERIES "," OCC1 "," DENTIST "," PARTY "]");
}

void tearDown(void) {
    cJSON_Delete(F.events);
    cJSON_Delete(F.last_body);
}

static hp_form_t form_of(const char *id) {
    hp_event_details_t d;
    char msg[160];
    TEST_ASSERT_EQUAL(HP_EDIT_OK, hp_edit_details(&OPS, &CFG, id, &d, msg, sizeof msg));
    return d.form;
}

static const char *body_str(const char *key) {
    const cJSON *i = cJSON_GetObjectItem(F.last_body, key);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

static void test_add(void) {
    hp_form_t f = {0};
    strcpy(f.title, "Pizza");
    f.date = (hp_date_t){2026, 10, 8};
    f.all_day = true;
    char msg[160];
    TEST_ASSERT_EQUAL(HP_EDIT_OK, hp_edit_add(&OPS, &CFG, &f, msg, sizeof msg));
    TEST_ASSERT_EQUAL_STRING("insert", F.last_op);
    TEST_ASSERT_EQUAL_STRING("Pizza", body_str("summary"));
    strcpy(f.title, " ");
    TEST_ASSERT_EQUAL(HP_EDIT_INVALID, hp_edit_add(&OPS, &CFG, &f, msg, sizeof msg));
    TEST_ASSERT_EQUAL_STRING("Please enter a title.", msg);
}

static void test_details_occurrence_uses_series_rule(void) {
    hp_event_details_t d;
    char msg[160];
    TEST_ASSERT_EQUAL(HP_EDIT_OK, hp_edit_details(&OPS, &CFG, "soccer_1013", &d, msg, sizeof msg));
    TEST_ASSERT_TRUE(d.recurring);
    TEST_ASSERT_EQUAL(HP_REPEAT_WEEKLY, d.form.repeat);
    TEST_ASSERT_EQUAL(HP_EDIT_NOT_FOUND, hp_edit_details(&OPS, &CFG, "nope", &d, msg, sizeof msg));
    TEST_ASSERT_NOT_NULL(strstr(msg, "no longer exists"));
}

static void test_edit_single_event(void) {
    hp_form_t f = form_of("dentist");
    strcpy(f.title, "Orthodontist");
    strcpy(f.member, "Sam");
    char msg[160];
    TEST_ASSERT_EQUAL(HP_EDIT_OK, hp_edit_update(&OPS, &CFG, "dentist", &f, false, msg, sizeof msg));
    TEST_ASSERT_EQUAL_STRING("update", F.last_op);
    TEST_ASSERT_EQUAL_STRING("dentist", F.last_id);
    TEST_ASSERT_EQUAL_STRING("Orthodontist", body_str("summary"));
    TEST_ASSERT_EQUAL_STRING("11", body_str("colorId"));
    // Notes weren't touched: the original HTML goes back unchanged.
    TEST_ASSERT_EQUAL_STRING("<div>Bring <b>card</b></div>", body_str("description"));
}

static void test_edited_notes_replace_html(void) {
    hp_form_t f = form_of("dentist");
    strcpy(f.description, "Bring card and ID");
    char msg[160];
    TEST_ASSERT_EQUAL(HP_EDIT_OK, hp_edit_update(&OPS, &CFG, "dentist", &f, false, msg, sizeof msg));
    TEST_ASSERT_EQUAL_STRING("Bring card and ID", body_str("description"));
}

static void test_edit_this_occurrence(void) {
    hp_form_t f = form_of("soccer_1013");
    f.start_min = 18 * 60, f.end_min = 19 * 60;
    f.repeat = HP_REPEAT_NONE;  // the form hides repeat for "this event"
    char msg[160];
    TEST_ASSERT_EQUAL(HP_EDIT_OK, hp_edit_update(&OPS, &CFG, "soccer_1013", &f, false, msg, sizeof msg));
    TEST_ASSERT_EQUAL_STRING("soccer_1013", F.last_id);
    TEST_ASSERT_EQUAL_STRING("soccer", body_str("recurringEventId"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(F.last_body, "recurrence"));
    TEST_ASSERT_EQUAL_STRING("2026-10-13T18:00:00", cJSON_GetObjectItem(cJSON_GetObjectItem(F.last_body, "start"), "dateTime")->valuestring);
}

static void test_edit_all_events_shifts_series(void) {
    hp_form_t f = form_of("soccer_1013");
    f.date = (hp_date_t){2026, 10, 14};  // moved the Oct 13 occurrence to Wednesday
    f.start_min = 18 * 60, f.end_min = 19 * 60 + 30;
    char msg[160];
    TEST_ASSERT_EQUAL(HP_EDIT_OK, hp_edit_update(&OPS, &CFG, "soccer_1013", &f, true, msg, sizeof msg));
    TEST_ASSERT_EQUAL_STRING("soccer", F.last_id);
    TEST_ASSERT_EQUAL_STRING("2026-10-07T18:00:00", cJSON_GetObjectItem(cJSON_GetObjectItem(F.last_body, "start"), "dateTime")->valuestring);
    cJSON *rec = cJSON_GetObjectItem(F.last_body, "recurrence");
    TEST_ASSERT_EQUAL_STRING("RRULE:FREQ=WEEKLY", cJSON_GetArrayItem(rec, 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("EXDATE;TZID=America/Los_Angeles:20261020T170000", cJSON_GetArrayItem(rec, 1)->valuestring);
}

static void test_edit_all_keeps_custom_rule(void) {
    cJSON *series = find(&F, "soccer");
    cJSON_ReplaceItemInObject(series, "recurrence", cJSON_Parse("[\"RRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=TU\"]"));
    hp_form_t f = form_of("soccer_1013");
    strcpy(f.title, "Soccer!");
    f.repeat = HP_REPEAT_DAILY;
    char msg[160];
    TEST_ASSERT_EQUAL(HP_EDIT_OK, hp_edit_update(&OPS, &CFG, "soccer_1013", &f, true, msg, sizeof msg));
    TEST_ASSERT_EQUAL_STRING("RRULE:FREQ=WEEKLY;INTERVAL=2;BYDAY=TU",
                             cJSON_GetArrayItem(cJSON_GetObjectItem(F.last_body, "recurrence"), 0)->valuestring);
}

static void test_delete_this_and_all(void) {
    char msg[160];
    TEST_ASSERT_EQUAL(HP_EDIT_OK, hp_edit_delete(&OPS, &CFG, "soccer_1013", false, msg, sizeof msg));
    TEST_ASSERT_EQUAL_STRING("soccer_1013", F.last_id);
    TEST_ASSERT_EQUAL(HP_EDIT_OK, hp_edit_delete(&OPS, &CFG, "soccer_1013", true, msg, sizeof msg));
    TEST_ASSERT_EQUAL_STRING("soccer", F.last_id);
    TEST_ASSERT_EQUAL(HP_EDIT_NOT_FOUND, hp_edit_delete(&OPS, &CFG, "gone", false, msg, sizeof msg));
}

static void test_guest_event_who_only(void) {
    hp_form_t f = form_of("party");
    strcpy(f.member, "Sam");
    char msg[160];
    TEST_ASSERT_EQUAL(HP_EDIT_OK, hp_edit_update(&OPS, &CFG, "party", &f, false, msg, sizeof msg));
    TEST_ASSERT_EQUAL_STRING("patch", F.last_op);
    TEST_ASSERT_EQUAL_INT(1, cJSON_GetArraySize(F.last_body));
    TEST_ASSERT_EQUAL_STRING("11", body_str("colorId"));

    f.member[0] = '\0';  // back to whole family: color cleared
    TEST_ASSERT_EQUAL(HP_EDIT_OK, hp_edit_update(&OPS, &CFG, "party", &f, false, msg, sizeof msg));
    TEST_ASSERT_TRUE(cJSON_IsNull(cJSON_GetObjectItem(F.last_body, "colorId")));

    int calls = F.calls;
    strcpy(f.title, "Party!");
    TEST_ASSERT_EQUAL(HP_EDIT_NOT_ALLOWED, hp_edit_update(&OPS, &CFG, "party", &f, false, msg, sizeof msg));
    TEST_ASSERT_NOT_NULL(strstr(msg, "created by someone else"));
    TEST_ASSERT_EQUAL_INT(calls, F.calls);  // nothing sent to Google

    TEST_ASSERT_EQUAL(HP_EDIT_NOT_ALLOWED, hp_edit_delete(&OPS, &CFG, "party", false, msg, sizeof msg));
    TEST_ASSERT_NOT_NULL(strstr(msg, "Only they can delete"));
}

static void test_offline(void) {
    F.offline = true;
    hp_form_t f = {0};
    strcpy(f.title, "x");
    f.all_day = true;
    f.date = (hp_date_t){2026, 10, 8};
    char msg[160];
    TEST_ASSERT_EQUAL(HP_EDIT_NETWORK, hp_edit_add(&OPS, &CFG, &f, msg, sizeof msg));
    TEST_ASSERT_NOT_NULL(strstr(msg, "Can't reach Google"));
    TEST_ASSERT_EQUAL(HP_EDIT_NETWORK, hp_edit_update(&OPS, &CFG, "dentist", &f, false, msg, sizeof msg));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_add);
    RUN_TEST(test_details_occurrence_uses_series_rule);
    RUN_TEST(test_edit_single_event);
    RUN_TEST(test_edited_notes_replace_html);
    RUN_TEST(test_edit_this_occurrence);
    RUN_TEST(test_edit_all_events_shifts_series);
    RUN_TEST(test_edit_all_keeps_custom_rule);
    RUN_TEST(test_delete_this_and_all);
    RUN_TEST(test_guest_event_who_only);
    RUN_TEST(test_offline);
    return UNITY_END();
}
