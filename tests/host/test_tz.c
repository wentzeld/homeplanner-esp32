// The POSIX TZ evaluator against Python's zoneinfo (fixtures/tz_fixture.h, tools/gen_tz_fixture.py).
#include <stdio.h>
#include <string.h>

#include "tz.h"
#include "tz_fixture.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define COUNT(a) ((int)(sizeof(a) / sizeof((a)[0])))

static void test_offsets_match_zoneinfo(void) {
    int checked = 0;
    for (int i = 0; i < COUNT(TZ_OFFSET_CASES); i++) {
        const tz_offset_case_t *c = &TZ_OFFSET_CASES[i];
        const char *posix = tz_posix_for(c->zone);
        TEST_ASSERT_NOT_NULL_MESSAGE(posix, c->zone);
        char msg[96];
        snprintf(msg, sizeof msg, "%s at %lld", c->zone, (long long)c->utc);
        TEST_ASSERT_EQUAL_INT_MESSAGE(c->offset, tz_utc_offset(posix, c->utc), msg);
        checked++;
    }
    TEST_ASSERT_GREATER_THAN(10000, checked);
}

static void test_local_to_utc_matches_zoneinfo(void) {
    for (int i = 0; i < COUNT(TZ_LOCAL_CASES); i++) {
        const tz_local_case_t *c = &TZ_LOCAL_CASES[i];
        char msg[96];
        snprintf(msg, sizeof msg, "%s %04d-%02d-%02d %02d:%02d", c->zone, c->y, c->m, c->d, c->hh, c->mm);
        TEST_ASSERT_EQUAL_INT64_MESSAGE(c->utc, tz_local_to_utc(tz_posix_for(c->zone), c->y, c->m, c->d, c->hh, c->mm, 0), msg);
    }
}

static void test_parse(void) {
    tz_rules_t tz;
    TEST_ASSERT_TRUE(tz_parse("PST8PDT,M3.2.0,M11.1.0", &tz));
    TEST_ASSERT_EQUAL_INT(-8 * 3600, tz.std_offset);
    TEST_ASSERT_EQUAL_INT(-7 * 3600, tz.dst_offset);
    TEST_ASSERT_TRUE(tz_parse("<+0545>-5:45", &tz));
    TEST_ASSERT_EQUAL_INT(5 * 3600 + 45 * 60, tz.std_offset);
    TEST_ASSERT_FALSE(tz.has_dst);
    TEST_ASSERT_TRUE(tz_parse("EST5EDT", &tz));  // no rule: US default
    TEST_ASSERT_EQUAL_INT(-4 * 3600, tz_rules_offset(&tz, 1782000000));  // July 2026
    TEST_ASSERT_TRUE(tz_parse("XXX3YYY,J60/1,300", &tz));
    TEST_ASSERT_FALSE(tz_parse("", &tz));
    TEST_ASSERT_FALSE(tz_parse("PST8PDT,M13.2.0,M11.1.0", &tz));
    TEST_ASSERT_FALSE(tz_parse("PST8PDT,M3.2.0", &tz));
    TEST_ASSERT_EQUAL_INT(0, tz_utc_offset("garbage", 0));
}

static void test_windows_names(void) {
    TEST_ASSERT_EQUAL_STRING("America/Los_Angeles", tz_windows_to_iana("Pacific Standard Time"));
    TEST_ASSERT_EQUAL_STRING("Europe/Berlin", tz_windows_to_iana("W. Europe Standard Time"));
    TEST_ASSERT_NULL(tz_windows_to_iana("Nowhere Standard Time"));
    TEST_ASSERT_NOT_NULL(tz_posix_for(tz_windows_to_iana("FLE Standard Time")));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_offsets_match_zoneinfo);
    RUN_TEST(test_local_to_utc_matches_zoneinfo);
    RUN_TEST(test_parse);
    RUN_TEST(test_windows_names);
    return UNITY_END();
}
