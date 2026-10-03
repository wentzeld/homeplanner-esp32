// Host tests for the setup page's settings validation and the timezone table.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "settings.h"
#include "tz.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

#define VALID                                                                                         \
    "{\"wifi_ssid\":\"Home\",\"wifi_pass\":\"secret123\",\"calendar_id\":\"fam@group.calendar.google.com\"," \
    "\"timezone\":\"America/Los_Angeles\",\"members\":[{\"name\":\"Elle\",\"color_id\":\"4\"},"           \
    "{\"name\":\"Sam\",\"color_id\":\"11\"}],\"latitude\":47.6,\"longitude\":-122.3,\"units\":\"fahrenheit\"," \
    "\"sleep_off\":\"22:00\",\"sleep_on\":\"06:30\",\"wake_minutes\":5}"

static void test_valid_settings_parse(void) {
    hp_settings_t s;
    bool has_pass;
    char err[128] = "";
    TEST_ASSERT_TRUE_MESSAGE(settings_from_json(VALID, &s, &has_pass, err, sizeof err), err);
    TEST_ASSERT_TRUE(has_pass);
    TEST_ASSERT_EQUAL_STRING("Home", s.wifi_ssid);
    TEST_ASSERT_EQUAL_STRING("secret123", s.wifi_pass);
    TEST_ASSERT_EQUAL_STRING("PST8PDT,M3.2.0,M11.1.0", s.tz_posix);
    TEST_ASSERT_EQUAL_INT(2, s.cal.member_count);
    TEST_ASSERT_EQUAL_STRING("Elle", s.cal.members[0].name);
    TEST_ASSERT_EQUAL_STRING("4", s.cal.members[0].color_id);
    TEST_ASSERT_TRUE(s.fahrenheit);
    TEST_ASSERT_EQUAL_INT(22 * 60, s.sleep_off_min);
    TEST_ASSERT_EQUAL_INT(6 * 60 + 30, s.sleep_on_min);
}

static void test_round_trip_without_password(void) {
    hp_settings_t s, back;
    bool has_pass;
    char err[128];
    settings_from_json(VALID, &s, &has_pass, err, sizeof err);
    char *json = settings_to_json(&s);
    TEST_ASSERT_NULL(strstr(json, "secret123"));  // the password is never sent back to the page
    TEST_ASSERT_TRUE_MESSAGE(settings_from_json(json, &back, &has_pass, err, sizeof err), err);
    TEST_ASSERT_FALSE(has_pass);
    TEST_ASSERT_EQUAL_STRING(s.cal.calendar_id, back.cal.calendar_id);
    TEST_ASSERT_EQUAL_INT(s.sleep_on_min, back.sleep_on_min);
    free(json);
}

static void expect_error(const char *from, const char *to, const char *fragment) {
    char json[1024];
    const char *p = strstr(VALID, from);
    TEST_ASSERT_NOT_NULL_MESSAGE(p, from);
    snprintf(json, sizeof json, "%.*s%s%s", (int)(p - VALID), VALID, to, p + strlen(from));
    hp_settings_t s;
    bool has_pass;
    char err[128] = "";
    TEST_ASSERT_FALSE_MESSAGE(settings_from_json(json, &s, &has_pass, err, sizeof err), json);
    TEST_ASSERT_NOT_NULL_MESSAGE(strstr(err, fragment), err);
}

static void test_validation_errors(void) {
    expect_error("\"wifi_ssid\":\"Home\"", "\"wifi_ssid\":\"\"", "Wi-Fi");
    expect_error("fam@group.calendar.google.com", "family-calendar", "calendar ID");
    expect_error("America/Los_Angeles", "Mars/Olympus", "timezone");
    expect_error("\"color_id\":\"11\"", "\"color_id\":\"4\"", "different color");
    expect_error("\"name\":\"Sam\"", "\"name\":\"Elle\"", "names must be different");
    expect_error("\"color_id\":\"4\"", "\"color_id\":\"12\"", "color");
    expect_error("\"latitude\":47.6", "\"latitude\":95", "latitude");
    expect_error("\"sleep_off\":\"22:00\"", "\"sleep_off\":\"25:00\"", "HH:MM");
    expect_error("\"wake_minutes\":5", "\"wake_minutes\":0", "Wake time");
    expect_error("\"members\":[{\"name\":\"Elle\",\"color_id\":\"4\"},{\"name\":\"Sam\",\"color_id\":\"11\"}]",
                 "\"members\":[]", "at least one");
    hp_settings_t s;
    bool has_pass;
    char err[128];
    TEST_ASSERT_FALSE(settings_from_json("not json", &s, &has_pass, err, sizeof err));
}

static void test_timezone_table(void) {
    TEST_ASSERT_EQUAL_STRING("PST8PDT,M3.2.0,M11.1.0", tz_posix_for("America/Los_Angeles"));
    TEST_ASSERT_EQUAL_STRING("SAST-2", tz_posix_for("Africa/Johannesburg"));
    TEST_ASSERT_EQUAL_STRING("UTC0", tz_posix_for("UTC"));
    TEST_ASSERT_NULL(tz_posix_for("Mars/Olympus"));
    TEST_ASSERT_NULL(tz_posix_for(NULL));
    TEST_ASSERT_TRUE(TZ_TABLE_COUNT > 300);
    for (int i = 1; i < TZ_TABLE_COUNT; i++) TEST_ASSERT_TRUE(strcmp(TZ_TABLE[i - 1].name, TZ_TABLE[i].name) < 0);
}

static bool parse(const char *json, hp_settings_t *s, char *err, size_t n) {
    bool has_pass;
    return settings_from_json(json, s, &has_pass, err, n);
}

#define BASE                                                                                          \
    "{\"wifi_ssid\":\"Home\",\"calendar_id\":\"fam@group.calendar.google.com\",\"timezone\":\"UTC\"," \
    "\"members\":[{\"name\":\"A\",\"color_id\":\"1\"}],\"sleep_off\":\"22:00\",\"sleep_on\":\"06:30\""

static void test_place_instead_of_coordinates(void) {
    hp_settings_t s;
    char err[128] = "";
    TEST_ASSERT_TRUE_MESSAGE(parse(BASE ",\"place\":\"  98101 \"}", &s, err, sizeof err), err);
    TEST_ASSERT_EQUAL_STRING("98101", s.place);
    TEST_ASSERT_FALSE(s.has_coords);  // looked up by the panel once online

    TEST_ASSERT_TRUE_MESSAGE(parse(BASE ",\"latitude\":47.6,\"longitude\":-122.3}", &s, err, sizeof err), err);
    TEST_ASSERT_TRUE(s.has_coords);
    TEST_ASSERT_EQUAL_STRING("", s.place);

    TEST_ASSERT_FALSE(parse(BASE "}", &s, err, sizeof err));
    TEST_ASSERT_NOT_NULL(strstr(err, "city"));
    TEST_ASSERT_FALSE(parse(BASE ",\"latitude\":95,\"longitude\":0}", &s, err, sizeof err));
    // The page sends null coordinates when only a place is given.
    TEST_ASSERT_TRUE(parse(BASE ",\"place\":\"Seattle\",\"latitude\":null,\"longitude\":null}", &s, err, sizeof err));
    TEST_ASSERT_FALSE(s.has_coords);
}

static void test_place_and_found_coordinates_round_trip(void) {
    hp_settings_t s, back;
    char err[128];
    parse(BASE ",\"place\":\"Seattle\"}", &s, err, sizeof err);
    s.latitude = 47.6062, s.longitude = -122.3321, s.has_coords = true;  // after the lookup
    char *json = settings_to_json(&s);
    TEST_ASSERT_TRUE_MESSAGE(parse(json, &back, err, sizeof err), err);
    TEST_ASSERT_EQUAL_STRING("Seattle", back.place);
    TEST_ASSERT_TRUE(back.has_coords);
    TEST_ASSERT_EQUAL_FLOAT(47.6062, back.latitude);
    free(json);
}

static void test_calendar_urls(void) {
    char out[HP_URL_LEN];
    TEST_ASSERT_NULL(calendar_normalize_url("  webcal://ics.example.com/school.ics?k=1#top \n", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("https://ics.example.com/school.ics?k=1", out);
    TEST_ASSERT_NULL(calendar_normalize_url("HTTP://Example.com", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("http://Example.com", out);
    TEST_ASSERT_NULL(calendar_normalize_url("webcals://x.example/a", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("https://x.example/a", out);
    TEST_ASSERT_NOT_NULL(calendar_normalize_url("ftp://example.com/a.ics", out, sizeof out));
    TEST_ASSERT_NOT_NULL(calendar_normalize_url("example.com/a.ics", out, sizeof out));
    TEST_ASSERT_NOT_NULL(calendar_normalize_url("https:///path", out, sizeof out));
    TEST_ASSERT_NOT_NULL(calendar_normalize_url("https://exa mple.com/a", out, sizeof out));
    TEST_ASSERT_NOT_NULL(calendar_normalize_url("", out, sizeof out));
    TEST_ASSERT_NOT_NULL(calendar_normalize_url(NULL, out, sizeof out));
    TEST_ASSERT_NOT_NULL(calendar_normalize_url("https://calendar.google.com/calendar/embed?src=abc%40group.calendar.google.com", out, sizeof out));
    TEST_ASSERT_NOT_NULL(calendar_normalize_url("https://calendar.google.com/calendar/u/0?cid=abc", out, sizeof out));
    TEST_ASSERT_NULL(calendar_normalize_url("https://calendar.google.com/calendar/ical/abc%40group.calendar.google.com/private-x/basic.ics", out, sizeof out));
    char small[16];
    TEST_ASSERT_EQUAL_STRING("That link is too long.", calendar_normalize_url("https://example.com/long", small, sizeof small));
}

static void test_calendar_check(void) {
    TEST_ASSERT_NULL(calendar_check("School", "cherry-blossom"));
    TEST_ASSERT_NULL(calendar_check("School", "4"));
    TEST_ASSERT_NOT_NULL(calendar_check("", "4"));
    TEST_ASSERT_NOT_NULL(calendar_check("School", "pink"));
    TEST_ASSERT_NOT_NULL(calendar_check("A name that is far too long for the legend of the panel", "4"));
}

static void test_calendars_round_trip(void) {
    hp_calendars_t c = {0}, back;
    c.count = 2;
    snprintf(c.items[0].id, sizeof c.items[0].id, "a1b2c3d4e5f6");
    snprintf(c.items[0].name, sizeof c.items[0].name, "School");
    snprintf(c.items[0].url, sizeof c.items[0].url, "https://example.com/school.ics");
    snprintf(c.items[0].color, sizeof c.items[0].color, "pumpkin");
    snprintf(c.items[1].id, sizeof c.items[1].id, "0f0f0f0f0f0f");
    snprintf(c.items[1].name, sizeof c.items[1].name, "Soccer \"club\"");
    snprintf(c.items[1].url, sizeof c.items[1].url, "http://example.org/x?y=1&z=2");
    snprintf(c.items[1].color, sizeof c.items[1].color, "7");
    char *json = calendars_to_json(&c);
    TEST_ASSERT_TRUE(calendars_from_json(json, &back));
    free(json);
    TEST_ASSERT_EQUAL_INT(2, back.count);
    TEST_ASSERT_EQUAL_MEMORY(&c, &back, sizeof c);
    TEST_ASSERT_EQUAL_STRING("Soccer \"club\"", calendars_find(&back, "0f0f0f0f0f0f")->name);
    TEST_ASSERT_NULL(calendars_find(&back, "nope"));

    // Bad entries are dropped, not the whole list; unreadable JSON is reported.
    TEST_ASSERT_TRUE(calendars_from_json("[{\"id\":\"x\",\"name\":\"A\",\"url\":\"https://a.example\",\"color\":\"1\"},"
                                         "{\"id\":\"y\",\"name\":\"B\",\"url\":\"ftp://b\",\"color\":\"1\"},"
                                         "{\"id\":\"z\",\"name\":\"C\",\"url\":\"https://c.example\",\"color\":\"nope\"}]", &back));
    TEST_ASSERT_EQUAL_INT(1, back.count);
    TEST_ASSERT_EQUAL_STRING("x", back.items[0].id);
    TEST_ASSERT_FALSE(calendars_from_json("{", &back));
    TEST_ASSERT_EQUAL_INT(0, back.count);
}

static void test_palette(void) {
    TEST_ASSERT_EQUAL_STRING("#d81b60", hp_palette_hex("cherry-blossom"));
    TEST_ASSERT_EQUAL_STRING(hp_color_hex("4"), hp_palette_hex("4"));
    TEST_ASSERT_NULL(hp_palette_hex("pink"));
    for (int i = 0; i < HP_PALETTE_COUNT; i++)
        for (int j = i + 1; j < HP_PALETTE_COUNT; j++) TEST_ASSERT_TRUE(strcmp(HP_PALETTE[i].id, HP_PALETTE[j].id) != 0);
}

static void test_wifi_only(void) {
    hp_wifi_t w;
    char err[128] = "";
    TEST_ASSERT_TRUE_MESSAGE(settings_wifi_from_json("{\"wifi_ssid\":\"Home\",\"wifi_pass\":\"pw\",\"timezone\":\"Europe/Amsterdam\"}",
                                                     &w, err, sizeof err), err);
    TEST_ASSERT_EQUAL_STRING("Home", w.ssid);
    TEST_ASSERT_TRUE(w.has_pass);
    TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", w.tz_posix);
    TEST_ASSERT_FALSE(settings_wifi_from_json("{\"wifi_ssid\":\"\",\"timezone\":\"UTC\"}", &w, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("Choose your home Wi-Fi network.", err);
    TEST_ASSERT_FALSE(settings_wifi_from_json("{\"wifi_ssid\":\"Home\",\"timezone\":\"Mars/Base\"}", &w, err, sizeof err));
    TEST_ASSERT_EQUAL_STRING("Choose your timezone.", err);

    // A new panel: Wi-Fi only, stored as incomplete, read back the same.
    TEST_ASSERT_TRUE(settings_wifi_from_json("{\"wifi_ssid\":\"Home\",\"wifi_pass\":\"pw\",\"timezone\":\"Europe/Amsterdam\"}",
                                             &w, err, sizeof err));
    hp_settings_t s = {0}, back;
    settings_apply_wifi(&s, &w);
    char *json = settings_to_json(&s);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"complete\":false"));
    TEST_ASSERT_NULL(strstr(json, "\"pw\""));  // never the password
    TEST_ASSERT_TRUE(settings_from_stored_json(json, &back));
    free(json);
    TEST_ASSERT_FALSE(back.complete);
    TEST_ASSERT_EQUAL_STRING("Home", back.wifi_ssid);
    TEST_ASSERT_EQUAL_STRING("Europe/Amsterdam", back.cal.timezone);
    TEST_ASSERT_EQUAL_STRING("CET-1CEST,M3.5.0,M10.5.0/3", back.tz_posix);
    TEST_ASSERT_EQUAL_INT(5, back.wake_minutes);
    TEST_ASSERT_FALSE(back.fahrenheit);
}

static void test_wifi_change_keeps_the_rest(void) {
    hp_settings_t s;
    bool has_pass;
    char err[128];
    TEST_ASSERT_TRUE(settings_from_json(VALID, &s, &has_pass, err, sizeof err));
    TEST_ASSERT_TRUE(s.complete);
    hp_wifi_t w = {.has_pass = false};
    snprintf(w.ssid, sizeof w.ssid, "Home");  // same network, password unchanged
    snprintf(w.timezone, sizeof w.timezone, "America/Los_Angeles");
    snprintf(w.tz_posix, sizeof w.tz_posix, "%s", tz_posix_for("America/Los_Angeles"));
    settings_apply_wifi(&s, &w);
    TEST_ASSERT_EQUAL_STRING("secret123", s.wifi_pass);
    TEST_ASSERT_TRUE(s.complete);
    TEST_ASSERT_EQUAL_INT(2, s.cal.member_count);
    snprintf(w.ssid, sizeof w.ssid, "Other");  // another network without a password: open
    settings_apply_wifi(&s, &w);
    TEST_ASSERT_EQUAL_STRING("", s.wifi_pass);
}

static void test_stored_full_settings(void) {
    hp_settings_t s, back;
    bool has_pass;
    char err[128];
    TEST_ASSERT_TRUE(settings_from_json(VALID, &s, &has_pass, err, sizeof err));
    char *json = settings_to_json(&s);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"complete\":true"));
    TEST_ASSERT_TRUE(settings_from_stored_json(json, &back));
    free(json);
    TEST_ASSERT_TRUE(back.complete);
    TEST_ASSERT_EQUAL_INT(2, back.cal.member_count);
    // Settings saved by older versions have no "complete": full settings.
    TEST_ASSERT_TRUE(settings_from_stored_json(VALID, &back));
    TEST_ASSERT_TRUE(back.complete);
    TEST_ASSERT_FALSE(settings_from_stored_json("{\"complete\":false}", &back));
    TEST_ASSERT_FALSE(settings_from_stored_json("nonsense", &back));
}

static void test_google_calendars_in_list(void) {
    hp_calendars_t c = {0}, back;
    c.count = 2;
    snprintf(c.items[0].id, sizeof c.items[0].id, "111111111111");
    snprintf(c.items[0].name, sizeof c.items[0].name, "School (Google)");
    c.items[0].google = true;
    snprintf(c.items[0].google_id, sizeof c.items[0].google_id, "abc123@group.calendar.google.com");
    snprintf(c.items[0].color, sizeof c.items[0].color, "cobalt");
    snprintf(c.items[1].id, sizeof c.items[1].id, "222222222222");
    snprintf(c.items[1].name, sizeof c.items[1].name, "Club");
    snprintf(c.items[1].url, sizeof c.items[1].url, "https://example.com/c.ics");
    snprintf(c.items[1].color, sizeof c.items[1].color, "7");
    char *json = calendars_to_json(&c);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"kind\":\"google\""));
    TEST_ASSERT_TRUE(calendars_from_json(json, &back));
    free(json);
    TEST_ASSERT_EQUAL_MEMORY(&c, &back, sizeof c);
    TEST_ASSERT_NULL(calendar_check_google_id("en.usa#holiday@group.v.calendar.google.com"));
    TEST_ASSERT_NOT_NULL(calendar_check_google_id(""));
    TEST_ASSERT_NOT_NULL(calendar_check_google_id("has space"));
    TEST_ASSERT_NOT_NULL(calendar_check_google_id(NULL));
    // A Google entry without an ID is dropped.
    TEST_ASSERT_TRUE(calendars_from_json("[{\"id\":\"x\",\"name\":\"A\",\"kind\":\"google\",\"color\":\"1\"}]", &back));
    TEST_ASSERT_EQUAL_INT(0, back.count);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_valid_settings_parse);
    RUN_TEST(test_round_trip_without_password);
    RUN_TEST(test_validation_errors);
    RUN_TEST(test_timezone_table);
    RUN_TEST(test_place_instead_of_coordinates);
    RUN_TEST(test_place_and_found_coordinates_round_trip);
    RUN_TEST(test_calendar_urls);
    RUN_TEST(test_calendar_check);
    RUN_TEST(test_calendars_round_trip);
    RUN_TEST(test_palette);
    RUN_TEST(test_wifi_only);
    RUN_TEST(test_wifi_change_keeps_the_rest);
    RUN_TEST(test_stored_full_settings);
    RUN_TEST(test_google_calendars_in_list);
    return UNITY_END();
}
