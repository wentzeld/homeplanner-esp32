// Host tests for components/logic, mirroring the Raspberry Pi version's Python tests
// (tests/test_events.py, test_edit_events.py, test_guest_events.py).
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hp_logic.h"
#include "unity.h"

static hp_config_t CFG;

void setUp(void) {
    setenv("TZ", "PST8PDT,M3.2.0,M11.1.0", 1);  // America/Los_Angeles
    tzset();
    memset(&CFG, 0, sizeof CFG);
    strcpy(CFG.calendar_id, "family@group.calendar.google.com");
    strcpy(CFG.timezone, "America/Los_Angeles");
    strcpy(CFG.members[0].name, "Alex");
    strcpy(CFG.members[0].color_id, "9");
    strcpy(CFG.members[1].name, "Sam");
    strcpy(CFG.members[1].color_id, "11");
    CFG.member_count = 2;
}

void tearDown(void) {}

static cJSON *J(const char *text) {
    cJSON *j = cJSON_Parse(text);
    TEST_ASSERT_NOT_NULL_MESSAGE(j, text);
    return j;
}

static hp_date_t D(int y, int m, int d) { return (hp_date_t){y, m, d}; }

static int64_t local(int y, int m, int d, int h, int mi) { return hp_local_midnight(D(y, m, d)) + h * 3600 + mi * 60; }

static const char *str(const cJSON *o, const char *k) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

static hp_form_t form_timed(const char *title, hp_date_t date, int start, int end) {
    hp_form_t f;
    memset(&f, 0, sizeof f);
    strcpy(f.title, title);
    f.date = date;
    f.start_min = start;
    f.end_min = end;
    return f;
}

static hp_form_t form_all_day(const char *title, hp_date_t date) {
    hp_form_t f = form_timed(title, date, -1, -1);
    f.all_day = true;
    return f;
}

// --- dates ---------------------------------------------------------------------------------

static void test_civil_roundtrip_and_weekday(void) {
    for (int64_t days = -800000; days < 800000; days += 997) {
        hp_date_t d = hp_civil_from_days(days);
        TEST_ASSERT_EQUAL_INT64(days, hp_days_from_civil(d));
    }
    TEST_ASSERT_EQUAL_INT(2, hp_weekday(D(2026, 10, 6)));  // a Tuesday
    hp_date_t sunday = hp_week_start(D(2026, 10, 1));
    TEST_ASSERT_EQUAL_INT(0, hp_date_cmp(sunday, D(2026, 9, 27)));
    TEST_ASSERT_EQUAL_INT(0, hp_date_cmp(hp_date_add(D(2026, 2, 27), 2), D(2026, 3, 1)));
}

static void test_rfc3339(void) {
    int64_t t;
    TEST_ASSERT_TRUE(hp_parse_rfc3339("2026-10-05T17:00:00Z", &t));
    TEST_ASSERT_EQUAL_INT64(local(2026, 10, 5, 10, 0), t);
    TEST_ASSERT_TRUE(hp_parse_rfc3339("2026-10-05T10:00:00-07:00", &t));
    TEST_ASSERT_EQUAL_INT64(local(2026, 10, 5, 10, 0), t);
    TEST_ASSERT_TRUE(hp_parse_rfc3339("2026-10-05T19:00:00.123+02:00", &t));
    TEST_ASSERT_EQUAL_INT64(local(2026, 10, 5, 10, 0), t);
    TEST_ASSERT_FALSE(hp_parse_rfc3339("2026-10-05T17:00:00", &t));
    TEST_ASSERT_FALSE(hp_parse_rfc3339("garbage", &t));
}

// --- Google -> display -----------------------------------------------------------------------

static void test_timed_event_local_time_member_color(void) {
    cJSON *ev = J("{\"id\":\"a\",\"summary\":\"Soccer\",\"colorId\":\"11\",\"location\":\"Park\","
                  "\"start\":{\"dateTime\":\"2026-10-05T17:00:00Z\"},\"end\":{\"dateTime\":\"2026-10-05T18:30:00Z\"}}");
    hp_event_t e;
    TEST_ASSERT_TRUE(hp_event_from_google(ev, &CFG, &e));
    TEST_ASSERT_EQUAL_INT64(local(2026, 10, 5, 10, 0), e.start);
    TEST_ASSERT_EQUAL_INT64(local(2026, 10, 5, 11, 30), e.end);
    TEST_ASSERT_FALSE(e.all_day);
    TEST_ASSERT_EQUAL_STRING("Sam", e.member);
    TEST_ASSERT_EQUAL_STRING("#d50000", e.color);
    TEST_ASSERT_EQUAL_STRING("Park", e.location);
    TEST_ASSERT_TRUE(e.editable && e.owned && !e.recurring);
    cJSON_Delete(ev);
}

static void test_all_day_untagged_cancelled_untitled(void) {
    hp_event_t e;
    cJSON *ev = J("{\"id\":\"b\",\"summary\":\"Trip\",\"start\":{\"date\":\"2026-10-05\"},\"end\":{\"date\":\"2026-10-08\"}}");
    TEST_ASSERT_TRUE(hp_event_from_google(ev, &CFG, &e));
    TEST_ASSERT_TRUE(e.all_day);
    TEST_ASSERT_EQUAL_INT64(hp_local_midnight(D(2026, 10, 5)), e.start);
    TEST_ASSERT_EQUAL_INT64(hp_local_midnight(D(2026, 10, 8)), e.end);
    TEST_ASSERT_EQUAL_STRING("", e.member);
    TEST_ASSERT_EQUAL_STRING(HP_FAMILY_COLOR, e.color);
    cJSON_Delete(ev);

    ev = J("{\"id\":\"c\",\"colorId\":\"3\",\"start\":{\"date\":\"2026-10-05\"},\"end\":{\"date\":\"2026-10-06\"}}");
    TEST_ASSERT_TRUE(hp_event_from_google(ev, &CFG, &e));
    TEST_ASSERT_EQUAL_STRING("(No title)", e.title);
    TEST_ASSERT_EQUAL_STRING("#8e24aa", e.color);  // a color nobody uses: kept, no member
    TEST_ASSERT_EQUAL_STRING("", e.member);
    cJSON_Delete(ev);

    ev = J("{\"id\":\"d\",\"status\":\"cancelled\",\"start\":{\"date\":\"2026-10-05\"},\"end\":{\"date\":\"2026-10-06\"}}");
    TEST_ASSERT_FALSE(hp_event_from_google(ev, &CFG, &e));
    cJSON_Delete(ev);
}

static void test_overlaps_day(void) {
    hp_event_t e = {0};
    e.start = local(2026, 10, 5, 20, 0);
    e.end = hp_local_midnight(D(2026, 10, 6));  // ends exactly at midnight
    TEST_ASSERT_TRUE(hp_overlaps_day(&e, D(2026, 10, 5)));
    TEST_ASSERT_FALSE(hp_overlaps_day(&e, D(2026, 10, 6)));
    e.end = local(2026, 10, 6, 2, 0);  // overnight
    TEST_ASSERT_TRUE(hp_overlaps_day(&e, D(2026, 10, 6)));
}

static void test_ownership(void) {
    cJSON *guest = J("{\"organizer\":{\"email\":\"parent@example.com\"}}");
    cJSON *own_self = J("{\"organizer\":{\"email\":\"x\",\"self\":true}}");
    cJSON *own_email = J("{\"organizer\":{\"email\":\"family@group.calendar.google.com\"}}");
    cJSON *none = J("{\"id\":\"x\"}");
    TEST_ASSERT_FALSE(hp_is_owned(guest, &CFG));
    TEST_ASSERT_TRUE(hp_is_owned(own_self, &CFG));
    TEST_ASSERT_TRUE(hp_is_owned(own_email, &CFG));
    TEST_ASSERT_TRUE(hp_is_owned(none, &CFG));
    cJSON_Delete(guest), cJSON_Delete(own_self), cJSON_Delete(own_email), cJSON_Delete(none);
}

// --- form ----------------------------------------------------------------------------------

static void test_form_validation(void) {
    hp_form_t f = form_timed("Soccer", D(2026, 10, 5), 17 * 60, 18 * 60 + 30);
    TEST_ASSERT_NULL(hp_form_validate(&f));
    strcpy(f.title, "   ");
    TEST_ASSERT_NOT_NULL(hp_form_validate(&f));
    f = form_timed("Soccer", D(2026, 10, 5), 17 * 60, 17 * 60);
    TEST_ASSERT_NOT_NULL(hp_form_validate(&f));
    f = form_timed("Soccer", D(2026, 10, 5), -1, 18 * 60);
    TEST_ASSERT_NOT_NULL(hp_form_validate(&f));
    f = form_all_day("Trip", D(2026, 10, 5));
    TEST_ASSERT_NULL(hp_form_validate(&f));
    f.has_until = true, f.until = D(2026, 12, 1);  // until without repeat
    TEST_ASSERT_NOT_NULL(hp_form_validate(&f));
    f.repeat = HP_REPEAT_WEEKLY, f.until = D(2026, 10, 1);  // until before date
    TEST_ASSERT_NOT_NULL(hp_form_validate(&f));
}

static void test_timed_payload(void) {
    hp_form_t f = form_timed("  Soccer ", D(2026, 10, 5), 17 * 60, 18 * 60 + 30);
    strcpy(f.member, "Alex");
    strcpy(f.location, "Park");
    strcpy(f.description, "Bring water");
    const char *err = NULL;
    cJSON *p = hp_form_to_google(&f, &CFG, &err);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_STRING("Soccer", str(p, "summary"));
    const cJSON *start = cJSON_GetObjectItem(p, "start");
    TEST_ASSERT_EQUAL_STRING("2026-10-05T17:00:00", str(start, "dateTime"));
    TEST_ASSERT_EQUAL_STRING("America/Los_Angeles", str(start, "timeZone"));
    TEST_ASSERT_EQUAL_STRING("2026-10-05T18:30:00", str(cJSON_GetObjectItem(p, "end"), "dateTime"));
    TEST_ASSERT_EQUAL_STRING("9", str(p, "colorId"));
    TEST_ASSERT_EQUAL_STRING("Park", str(p, "location"));
    TEST_ASSERT_EQUAL_STRING("Bring water", str(p, "description"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(p, "recurrence"));
    cJSON_Delete(p);
}

static void test_all_day_payload_and_unknown_member(void) {
    hp_form_t f = form_all_day("Trip", D(2026, 10, 5));
    cJSON *p = hp_form_to_google(&f, &CFG, NULL);
    TEST_ASSERT_EQUAL_STRING("2026-10-05", str(cJSON_GetObjectItem(p, "start"), "date"));
    TEST_ASSERT_EQUAL_STRING("2026-10-06", str(cJSON_GetObjectItem(p, "end"), "date"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(p, "colorId"));
    cJSON_Delete(p);
    strcpy(f.member, "Nobody");
    const char *err = NULL;
    TEST_ASSERT_NULL(hp_form_to_google(&f, &CFG, &err));
    TEST_ASSERT_NOT_NULL(err);
}

static const char *first_rule(const cJSON *p) {
    return cJSON_GetArrayItem(cJSON_GetObjectItem(p, "recurrence"), 0)->valuestring;
}

static void test_recurrence_payloads(void) {
    const char *names[] = {"DAILY", "WEEKLY", "MONTHLY", "YEARLY"};
    for (int r = HP_REPEAT_DAILY; r <= HP_REPEAT_YEARLY; r++) {
        hp_form_t f = form_timed("x", D(2026, 10, 5), 600, 660);
        f.repeat = (hp_repeat_t)r;
        cJSON *p = hp_form_to_google(&f, &CFG, NULL);
        char want[40];
        snprintf(want, sizeof want, "RRULE:FREQ=%s", names[r - 1]);
        TEST_ASSERT_EQUAL_STRING(want, first_rule(p));
        cJSON_Delete(p);
    }
    hp_form_t f = form_timed("x", D(2026, 10, 5), 600, 660);
    f.repeat = HP_REPEAT_WEEKLY, f.has_until = true, f.until = D(2026, 12, 14);
    cJSON *p = hp_form_to_google(&f, &CFG, NULL);
    // 2026-12-14 23:59:59 PST (UTC-8) == 2026-12-15 07:59:59 UTC
    TEST_ASSERT_EQUAL_STRING("RRULE:FREQ=WEEKLY;UNTIL=20261215T075959Z", first_rule(p));
    cJSON_Delete(p);
    hp_form_t a = form_all_day("x", D(2026, 10, 5));
    a.repeat = HP_REPEAT_YEARLY, a.has_until = true, a.until = D(2030, 10, 5);
    p = hp_form_to_google(&a, &CFG, NULL);
    TEST_ASSERT_EQUAL_STRING("RRULE:FREQ=YEARLY;UNTIL=20301005", first_rule(p));
    cJSON_Delete(p);
}

// --- repeat rules ----------------------------------------------------------------------------

static void check_rule(const char *json, bool simple, hp_repeat_t repeat, bool has_until, hp_date_t until) {
    cJSON *rec = J(json);
    hp_repeat_t r;
    bool hu;
    hp_date_t u = {0};
    bool ok = hp_parse_simple_rrule(rec, D(2026, 10, 6), &r, &hu, &u);  // Oct 6 2026 = Tuesday
    TEST_ASSERT_EQUAL_MESSAGE(simple, ok, json);
    if (simple) {
        TEST_ASSERT_EQUAL_INT_MESSAGE(repeat, r, json);
        TEST_ASSERT_EQUAL_MESSAGE(has_until, hu, json);
        if (has_until) TEST_ASSERT_EQUAL_INT_MESSAGE(0, hp_date_cmp(until, u), json);
    }
    cJSON_Delete(rec);
}

static void test_simple_rules(void) {
    hp_date_t none = {0};
    check_rule("[]", true, HP_REPEAT_NONE, false, none);
    check_rule("[\"RRULE:FREQ=WEEKLY\"]", true, HP_REPEAT_WEEKLY, false, none);
    check_rule("[\"RRULE:FREQ=WEEKLY;BYDAY=TU\"]", true, HP_REPEAT_WEEKLY, false, none);
    check_rule("[\"RRULE:FREQ=WEEKLY;WKST=SU;BYDAY=TU\"]", true, HP_REPEAT_WEEKLY, false, none);
    check_rule("[\"rrule:freq=weekly;byday=tu\"]", true, HP_REPEAT_WEEKLY, false, none);
    check_rule("[\"RRULE:FREQ=MONTHLY;BYMONTHDAY=6\"]", true, HP_REPEAT_MONTHLY, false, none);
    check_rule("[\"RRULE:FREQ=DAILY;UNTIL=20261215\"]", true, HP_REPEAT_DAILY, true, D(2026, 12, 15));
    check_rule("[\"RRULE:FREQ=WEEKLY;UNTIL=20261215T075959Z\"]", true, HP_REPEAT_WEEKLY, true, D(2026, 12, 14));
    check_rule("[\"RRULE:FREQ=YEARLY\", \"EXDATE:20271006\"]", true, HP_REPEAT_YEARLY, false, none);
}

static void test_custom_rules(void) {
    hp_date_t none = {0};
    const char *custom[] = {
        "[\"RRULE:FREQ=WEEKLY;INTERVAL=2\"]", "[\"RRULE:FREQ=WEEKLY;BYDAY=TU,TH\"]",
        "[\"RRULE:FREQ=WEEKLY;BYDAY=WE\"]",   "[\"RRULE:FREQ=MONTHLY;BYDAY=2TU\"]",
        "[\"RRULE:FREQ=DAILY;COUNT=5\"]",     "[\"RRULE:FREQ=HOURLY\"]",
        "[\"RRULE:FREQ=DAILY\", \"RRULE:FREQ=WEEKLY\"]", "[\"RRULE:FREQ=DAILY;BYDAY=TU\"]",
    };
    for (size_t i = 0; i < sizeof custom / sizeof custom[0]; i++) check_rule(custom[i], false, HP_REPEAT_NONE, false, none);
}

// --- editing ---------------------------------------------------------------------------------

#define SERIES \
    "{\"id\":\"soccer\",\"summary\":\"Soccer\",\"colorId\":\"11\",\"location\":\"Park\",\"etag\":\"\\\"1\\\"\"," \
    "\"iCalUID\":\"soccer@google.com\",\"start\":{\"dateTime\":\"2026-10-06T17:00:00-07:00\",\"timeZone\":\"America/Los_Angeles\"}," \
    "\"end\":{\"dateTime\":\"2026-10-06T18:30:00-07:00\",\"timeZone\":\"America/Los_Angeles\"}," \
    "\"recurrence\":[\"RRULE:FREQ=WEEKLY;BYDAY=TU\",\"EXDATE;TZID=America/Los_Angeles:20261020T170000\"]}"
#define OCC1 \
    "{\"id\":\"soccer_20261013T000000Z\",\"summary\":\"Soccer\",\"colorId\":\"11\",\"location\":\"Park\"," \
    "\"recurringEventId\":\"soccer\",\"originalStartTime\":{\"dateTime\":\"2026-10-13T17:00:00-07:00\"}," \
    "\"start\":{\"dateTime\":\"2026-10-13T17:00:00-07:00\"},\"end\":{\"dateTime\":\"2026-10-13T18:30:00-07:00\"}}"
#define DENTIST \
    "{\"id\":\"dentist\",\"summary\":\"Dentist\",\"colorId\":\"9\",\"location\":\"Main St\",\"description\":\"Bring card\"," \
    "\"etag\":\"\\\"1\\\"\",\"iCalUID\":\"dentist@google.com\"," \
    "\"start\":{\"dateTime\":\"2026-10-07T15:30:00-07:00\"},\"end\":{\"dateTime\":\"2026-10-07T16:15:00-07:00\"}}"
#define TRIP "{\"id\":\"trip\",\"summary\":\"Grandma visiting\",\"start\":{\"date\":\"2026-10-08\"},\"end\":{\"date\":\"2026-10-11\"}}"

static void test_event_to_form_single(void) {
    cJSON *ev = J(DENTIST);
    hp_event_details_t d;
    TEST_ASSERT_TRUE(hp_event_to_form(ev, NULL, &CFG, &d));
    TEST_ASSERT_EQUAL_STRING("Dentist", d.form.title);
    TEST_ASSERT_EQUAL_INT(0, hp_date_cmp(d.form.date, D(2026, 10, 7)));
    TEST_ASSERT_EQUAL_INT(15 * 60 + 30, d.form.start_min);
    TEST_ASSERT_EQUAL_INT(16 * 60 + 15, d.form.end_min);
    TEST_ASSERT_EQUAL_STRING("Alex", d.form.member);
    TEST_ASSERT_EQUAL_STRING("Main St", d.form.location);
    TEST_ASSERT_EQUAL_STRING("Bring card", d.form.description);
    TEST_ASSERT_EQUAL_INT(HP_REPEAT_NONE, d.form.repeat);
    TEST_ASSERT_FALSE(d.recurring || d.custom_repeat);
    TEST_ASSERT_TRUE(d.form_editable && d.owned);
    cJSON_Delete(ev);
}

static void test_event_to_form_occurrence_uses_series_rule(void) {
    cJSON *occ = J(OCC1), *series = J(SERIES);
    hp_event_details_t d;
    TEST_ASSERT_TRUE(hp_event_to_form(occ, series, &CFG, &d));
    TEST_ASSERT_TRUE(d.recurring);
    TEST_ASSERT_EQUAL_INT(HP_REPEAT_WEEKLY, d.form.repeat);
    TEST_ASSERT_EQUAL_INT(0, hp_date_cmp(d.form.date, D(2026, 10, 13)));
    TEST_ASSERT_EQUAL_STRING("Sam", d.form.member);
    cJSON_Delete(occ), cJSON_Delete(series);
}

static void test_event_to_form_spans(void) {
    cJSON *trip = J(TRIP);
    hp_event_details_t d;
    hp_event_to_form(trip, NULL, &CFG, &d);
    TEST_ASSERT_EQUAL_INT(3, d.span_days);
    cJSON_Delete(trip);
    cJSON *overnight = J("{\"id\":\"p\",\"start\":{\"dateTime\":\"2026-10-07T20:00:00-07:00\"},\"end\":{\"dateTime\":\"2026-10-08T02:00:00-07:00\"}}");
    hp_event_to_form(overnight, NULL, &CFG, &d);
    TEST_ASSERT_FALSE(d.form_editable);
    cJSON_Delete(overnight);
    cJSON *to_midnight = J("{\"id\":\"l\",\"start\":{\"dateTime\":\"2026-10-07T20:00:00-07:00\"},\"end\":{\"dateTime\":\"2026-10-08T00:00:00-07:00\"}}");
    hp_event_to_form(to_midnight, NULL, &CFG, &d);
    TEST_ASSERT_TRUE(d.form_editable);
    cJSON_Delete(to_midnight);
}

static void test_apply_form_clears_fields_and_keeps_others(void) {
    cJSON *res = J(DENTIST);
    hp_form_t f = form_timed("Dentist", D(2026, 10, 7), 16 * 60, 17 * 60);  // member/location/notes cleared
    cJSON *body = hp_apply_form(res, &f, &CFG, false, 1, NULL);
    TEST_ASSERT_EQUAL_STRING("2026-10-07T16:00:00", str(cJSON_GetObjectItem(body, "start"), "dateTime"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(body, "colorId"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(body, "location"));
    TEST_ASSERT_NULL(cJSON_GetObjectItem(body, "description"));
    TEST_ASSERT_EQUAL_STRING("dentist@google.com", str(body, "iCalUID"));
    TEST_ASSERT_EQUAL_STRING("\"1\"", str(body, "etag"));
    cJSON_Delete(body), cJSON_Delete(res);
}

static void test_apply_form_recurrence_handling(void) {
    cJSON *series = J(SERIES);
    hp_form_t f = form_timed("Soccer", D(2026, 10, 6), 17 * 60, 18 * 60 + 30);
    strcpy(f.member, "Sam");
    f.repeat = HP_REPEAT_DAILY;
    cJSON *body = hp_apply_form(series, &f, &CFG, false, 1, NULL);
    cJSON *rec = cJSON_GetObjectItem(body, "recurrence");
    TEST_ASSERT_EQUAL_INT(2, cJSON_GetArraySize(rec));
    TEST_ASSERT_EQUAL_STRING("RRULE:FREQ=DAILY", cJSON_GetArrayItem(rec, 0)->valuestring);
    TEST_ASSERT_EQUAL_STRING("EXDATE;TZID=America/Los_Angeles:20261020T170000", cJSON_GetArrayItem(rec, 1)->valuestring);
    cJSON_Delete(body);

    f.repeat = HP_REPEAT_NONE;  // removing the repeat makes it a single event
    body = hp_apply_form(series, &f, &CFG, false, 1, NULL);
    TEST_ASSERT_NULL(cJSON_GetObjectItem(body, "recurrence"));
    cJSON_Delete(body);

    f.repeat = HP_REPEAT_DAILY;  // keep_recurrence leaves the rule untouched
    body = hp_apply_form(series, &f, &CFG, true, 1, NULL);
    TEST_ASSERT_EQUAL_STRING("RRULE:FREQ=WEEKLY;BYDAY=TU", cJSON_GetArrayItem(cJSON_GetObjectItem(body, "recurrence"), 0)->valuestring);
    cJSON_Delete(body), cJSON_Delete(series);
}

static void test_apply_form_keeps_multi_day_length(void) {
    cJSON *trip = J(TRIP);
    hp_form_t f = form_all_day("Grandma", D(2026, 10, 9));
    cJSON *body = hp_apply_form(trip, &f, &CFG, false, hp_all_day_span(trip), NULL);
    TEST_ASSERT_EQUAL_STRING("2026-10-09", str(cJSON_GetObjectItem(body, "start"), "date"));
    TEST_ASSERT_EQUAL_STRING("2026-10-12", str(cJSON_GetObjectItem(body, "end"), "date"));
    cJSON_Delete(body), cJSON_Delete(trip);
}

static void test_occurrence_date_uses_original_start(void) {
    cJSON *moved = J("{\"id\":\"m\",\"originalStartTime\":{\"dateTime\":\"2026-10-13T17:00:00-07:00\"},"
                     "\"start\":{\"dateTime\":\"2026-10-15T17:00:00-07:00\"},\"end\":{\"dateTime\":\"2026-10-15T18:00:00-07:00\"}}");
    TEST_ASSERT_EQUAL_INT(0, hp_date_cmp(hp_occurrence_date(moved), D(2026, 10, 13)));
    cJSON_Delete(moved);
}

static void test_guest_changes(void) {
    cJSON *party = J("{\"id\":\"party\",\"summary\":\"Birthday party\",\"colorId\":\"9\",\"location\":\"Park\","
                     "\"organizer\":{\"email\":\"parent@example.com\"},"
                     "\"start\":{\"dateTime\":\"2026-10-09T15:00:00-07:00\"},\"end\":{\"dateTime\":\"2026-10-09T17:00:00-07:00\"}}");
    hp_event_details_t d;
    hp_event_to_form(party, NULL, &CFG, &d);
    TEST_ASSERT_FALSE(d.owned);
    hp_form_t f = d.form;
    strcpy(f.member, "Sam");  // only who changes
    TEST_ASSERT_EQUAL_INT(0, hp_guest_changes(&d.form, &f));
    strcpy(f.title, "Party!");
    TEST_ASSERT_EQUAL_INT(HP_CHG_TITLE, hp_guest_changes(&d.form, &f));
    f = d.form;
    f.start_min += 30;
    TEST_ASSERT_EQUAL_INT(HP_CHG_TIME, hp_guest_changes(&d.form, &f));
    f = d.form;
    strcpy(f.location, "Home");
    strcpy(f.description, "Bring gift");
    TEST_ASSERT_EQUAL_INT(HP_CHG_LOCATION | HP_CHG_NOTES, hp_guest_changes(&d.form, &f));
    cJSON_Delete(party);
}

static void test_html_to_text(void) {
    char out[256];
    hp_html_to_text("Bring <b>water</b> &amp; snacks", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("Bring water & snacks", out);
    hp_html_to_text("<blockquote>Line one<br>Line two</blockquote><div>Next</div><div><br></div>", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("Line one\nLine two\nNext", out);
    hp_html_to_text("<ul><li>Shoes</li><li>Shin guards</li></ul>", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("- Shoes\n- Shin guards", out);
    hp_html_to_text("a &lt;b&gt; &quot;c&quot; &#39;d&#39; e&nbsp;f &#233;", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("a <b> \"c\" 'd' e f \xc3\xa9", out);
    hp_html_to_text("<a href=\"https://x.y/z\">Zoom link</a>", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("Zoom link", out);
    hp_html_to_text("Plain text\nwith newline", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("Plain text\nwith newline", out);
    hp_html_to_text("<p>One</p><p></p><p></p><p>Two</p>", out, sizeof out);
    TEST_ASSERT_EQUAL_STRING("One\n\nTwo", out);  // at most one blank line
    hp_html_to_text("x < y and 3 > 2", out, sizeof out);  // not a tag: kept
    TEST_ASSERT_EQUAL_STRING("x < y and 3 > 2", out);
    char tiny[8];
    hp_html_to_text("<b>truncate me please</b>", tiny, sizeof tiny);
    TEST_ASSERT_EQUAL_STRING("truncat", tiny);
}

static void test_event_description_shown_as_text(void) {
    cJSON *ev = J("{\"id\":\"n\",\"summary\":\"Party\",\"description\":\"<div>Bring <b>cake</b><br>RSVP</div>\","
                  "\"start\":{\"date\":\"2026-10-05\"},\"end\":{\"date\":\"2026-10-06\"}}");
    hp_event_t e;
    TEST_ASSERT_TRUE(hp_event_from_google(ev, &CFG, &e));
    TEST_ASSERT_EQUAL_STRING("Bring cake\nRSVP", e.description);
    hp_event_details_t d;
    hp_event_to_form(ev, NULL, &CFG, &d);
    TEST_ASSERT_EQUAL_STRING("Bring cake\nRSVP", d.form.description);
    cJSON_Delete(ev);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_civil_roundtrip_and_weekday);
    RUN_TEST(test_rfc3339);
    RUN_TEST(test_timed_event_local_time_member_color);
    RUN_TEST(test_all_day_untagged_cancelled_untitled);
    RUN_TEST(test_overlaps_day);
    RUN_TEST(test_ownership);
    RUN_TEST(test_form_validation);
    RUN_TEST(test_timed_payload);
    RUN_TEST(test_all_day_payload_and_unknown_member);
    RUN_TEST(test_recurrence_payloads);
    RUN_TEST(test_simple_rules);
    RUN_TEST(test_custom_rules);
    RUN_TEST(test_event_to_form_single);
    RUN_TEST(test_event_to_form_occurrence_uses_series_rule);
    RUN_TEST(test_event_to_form_spans);
    RUN_TEST(test_apply_form_clears_fields_and_keeps_others);
    RUN_TEST(test_apply_form_recurrence_handling);
    RUN_TEST(test_apply_form_keeps_multi_day_length);
    RUN_TEST(test_occurrence_date_uses_original_start);
    RUN_TEST(test_guest_changes);
    RUN_TEST(test_html_to_text);
    RUN_TEST(test_event_description_shown_as_text);
    return UNITY_END();
}
