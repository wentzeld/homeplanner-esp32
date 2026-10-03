// The iCal expander against the Pi version (fixtures/ical_fixture.h, tools/gen_ical_fixture.py),
// plus parsing details.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "ical.h"
#include "ical_fixture.h"
#include "tz.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static char *read_file(const char *name, size_t *len) {
    char path[512];
    snprintf(path, sizeof path, "%s/%s", ICAL_DIR, name);
    FILE *f = fopen(path, "rb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, path);
    fseek(f, 0, SEEK_END);
    *len = (size_t)ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = malloc(*len + 1);
    TEST_ASSERT_EQUAL_size_t(*len, fread(buf, 1, *len, f));
    buf[*len] = '\0';
    fclose(f);
    return buf;
}

static ical_cal_t *load(const char *name) {
    size_t len;
    char *text = read_file(name, &len);
    ical_status_t st;
    ical_cal_t *cal = ical_parse(text, len, &st);
    free(text);
    TEST_ASSERT_EQUAL_INT(ICAL_OK, st);
    TEST_ASSERT_NOT_NULL(cal);
    return cal;
}

typedef struct {
    hp_event_t ev[400];
    int n;
} collected_t;

static bool collect(const ical_cal_t *cal, const ical_occurrence_t *occ, void *ctx) {
    collected_t *c = ctx;
    TEST_ASSERT_LESS_THAN(400, c->n);
    ical_occurrence_to_event(cal, occ, "t", "Test", "#7986cb", &c->ev[c->n++]);
    return true;
}

static bool unsupported_title(const char *t) { return strncmp(t, "Unsupported", 11) == 0; }

static int cmp_event(const void *a, const void *b) {  // Python's tuple order in the generator
    const hp_event_t *x = a, *y = b;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    if (x->end != y->end) return x->end < y->end ? -1 : 1;
    if (x->all_day != y->all_day) return x->all_day ? 1 : -1;
    int c = strcmp(x->title, y->title);
    return c ? c : strcmp(x->location, y->location);
}

static void test_matches_pi_version(void) {
    static collected_t got;
    int total = 0;
    for (size_t i = 0; i < sizeof ICAL_CASES / sizeof ICAL_CASES[0]; i++) {
        const ical_case_t *c = &ICAL_CASES[i];
        ical_cal_t *cal = load(c->feed);
        got.n = 0;
        ical_expand(cal, c->posix, (hp_date_t){c->y, c->m, c->d}, c->days, collect, &got);
        int kept = 0;
        for (int k = 0; k < got.n; k++) {
            if (!unsupported_title(got.ev[k].title)) got.ev[kept++] = got.ev[k];
        }
        got.n = kept;
        qsort(got.ev, (size_t)got.n, sizeof got.ev[0], cmp_event);

        int e = 0;
        for (const ical_expected_t *row = c->rows; row->title; row++) {
            if (unsupported_title(row->title)) continue;  // the Pi expands rules we show once
            char msg[256];
            snprintf(msg, sizeof msg, "%s in %s, expected #%d \"%s\" at %lld", c->feed, c->zone, e, row->title, (long long)row->start);
            TEST_ASSERT_LESS_THAN_MESSAGE(got.n, e, msg);
            const hp_event_t *g = &got.ev[e++];
            TEST_ASSERT_EQUAL_STRING_MESSAGE(row->title, g->title, msg);
            TEST_ASSERT_EQUAL_INT64_MESSAGE(row->start, g->start, msg);
            TEST_ASSERT_EQUAL_INT64_MESSAGE(row->end, g->end, msg);
            TEST_ASSERT_EQUAL_MESSAGE(row->all_day, g->all_day, msg);
            TEST_ASSERT_EQUAL_STRING_MESSAGE(row->location, g->location, msg);
        }
        char msg[128];
        snprintf(msg, sizeof msg, "%s in %s: extra occurrences (first: \"%s\")", c->feed, c->zone, e < got.n ? got.ev[e].title : "");
        TEST_ASSERT_EQUAL_INT_MESSAGE(e, got.n, msg);
        total += e;
        ical_free(cal);
    }
    TEST_ASSERT_GREATER_THAN(500, total);
}

static void test_unsupported_rule_shows_first_occurrence(void) {
    static collected_t got;
    ical_cal_t *cal = load("rules.ics");
    TEST_ASSERT_EQUAL_INT(1, ical_unsupported_count(cal));
    got.n = 0;
    ical_expand(cal, tz_posix_for("America/Los_Angeles"), (hp_date_t){2026, 8, 30}, 120, collect, &got);
    int n = 0;
    for (int i = 0; i < got.n; i++) {
        if (unsupported_title(got.ev[i].title)) {
            n++;
            TEST_ASSERT_EQUAL_INT64(1788796800, got.ev[i].start);  // 2026-09-07 16:00Z
        }
    }
    TEST_ASSERT_EQUAL_INT(1, n);
    ical_free(cal);
}

static void test_event_details(void) {
    static collected_t got;
    ical_cal_t *cal = load("school.ics");
    got.n = 0;
    ical_expand(cal, tz_posix_for("America/Los_Angeles"), (hp_date_t){2026, 9, 1}, 70, collect, &got);
    const hp_event_t *first = NULL, *night = NULL, *trip = NULL;
    for (int i = 0; i < got.n; i++) {
        if (strcmp(got.ev[i].title, "First day of school") == 0) first = &got.ev[i];
        if (strcmp(got.ev[i].title, "Back to school night") == 0) night = &got.ev[i];
        if (strcmp(got.ev[i].title, "Field trip") == 0) trip = &got.ev[i];
    }
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_EQUAL_STRING("Doors open at 8:15, bring a backpack.\nSee you there!", first->description);
    TEST_ASSERT_EQUAL_STRING("Test", first->calendar);
    TEST_ASSERT_EQUAL_STRING("#7986cb", first->color);
    TEST_ASSERT_FALSE(first->editable);
    TEST_ASSERT_FALSE(first->recurring);
    TEST_ASSERT_EQUAL_STRING("", first->member);
    TEST_ASSERT_EQUAL_STRING_LEN("ext-t-", first->id, 6);
    TEST_ASSERT_NOT_NULL(strstr(first->id, "first-day@school.example"));
    TEST_ASSERT_NOT_NULL(night);
    TEST_ASSERT_EQUAL_STRING("", night->description);  // the VALARM's DESCRIPTION is not the event's
    TEST_ASSERT_NOT_NULL(trip);
    TEST_ASSERT_EQUAL_STRING("Bring a packed lunch\n\nBus leaves 9:00", trip->description);
    ical_free(cal);
}

static void test_crlf_bom_and_not_a_calendar(void) {
    size_t len;
    char *text = read_file("weekly_tz.ics", &len);
    char *crlf = malloc(len * 2 + 4);
    size_t n = 0;
    crlf[n++] = (char)0xEF, crlf[n++] = (char)0xBB, crlf[n++] = (char)0xBF;
    for (size_t i = 0; i < len; i++) {
        if (text[i] == '\n') crlf[n++] = '\r';
        crlf[n++] = text[i];
    }
    ical_status_t st;
    ical_cal_t *a = ical_parse(text, len, &st), *b = ical_parse(crlf, n, &st);
    TEST_ASSERT_EQUAL_INT(ICAL_OK, st);
    TEST_ASSERT_EQUAL_INT(ical_event_count(a), ical_event_count(b));
    TEST_ASSERT_EQUAL_INT(7, ical_event_count(b));
    ical_free(a);
    ical_free(b);
    free(text);
    free(crlf);

    const char *html = "<!doctype html><html><body>Not found</body></html>";
    TEST_ASSERT_NULL(ical_parse(html, strlen(html), &st));
    TEST_ASSERT_EQUAL_INT(ICAL_NOT_CALENDAR, st);
    TEST_ASSERT_NULL(ical_parse("", 0, &st));
    TEST_ASSERT_EQUAL_INT(ICAL_NOT_CALENDAR, st);
    const char *empty = "BEGIN:VCALENDAR\r\nVERSION:2.0\r\nEND:VCALENDAR\r\n";
    ical_cal_t *c = ical_parse(empty, strlen(empty), &st);
    TEST_ASSERT_NOT_NULL(c);
    TEST_ASSERT_EQUAL_INT(0, ical_event_count(c));
    ical_free(c);
}

static bool stop_after_one(const ical_cal_t *cal, const ical_occurrence_t *occ, void *ctx) {
    (void)cal, (void)occ;
    (*(int *)ctx)++;
    return false;
}

static void test_visit_can_stop(void) {
    ical_cal_t *cal = load("rules.ics");
    int calls = 0;
    TEST_ASSERT_EQUAL_INT(1, ical_expand(cal, "UTC0", (hp_date_t){2026, 9, 1}, 60, stop_after_one, &calls));
    TEST_ASSERT_EQUAL_INT(1, calls);
    ical_free(cal);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_matches_pi_version);
    RUN_TEST(test_unsupported_rule_shows_first_occurrence);
    RUN_TEST(test_event_details);
    RUN_TEST(test_crlf_bom_and_not_a_calendar);
    RUN_TEST(test_visit_can_stop);
    return UNITY_END();
}
