// Host tests for Open-Meteo response parsing.
#include <string.h>

#include "unity.h"
#include "weather.h"

void setUp(void) {}
void tearDown(void) {}

static void test_place_found(void) {
    const char *json = "{\"results\":[{\"name\":\"Seattle\",\"latitude\":47.60621,\"longitude\":-122.33207,"
                       "\"country\":\"United States\",\"admin1\":\"Washington\"}],\"generationtime_ms\":0.5}";
    double lat, lon;
    char label[96];
    TEST_ASSERT_TRUE(weather_parse_place(json, &lat, &lon, label, sizeof label));
    TEST_ASSERT_EQUAL_FLOAT(47.60621, lat);
    TEST_ASSERT_EQUAL_FLOAT(-122.33207, lon);
    TEST_ASSERT_EQUAL_STRING("Seattle, Washington, United States", label);
}

static void test_place_without_region_and_duplicates(void) {
    const char *json = "{\"results\":[{\"name\":\"Singapore\",\"latitude\":1.29,\"longitude\":103.85,"
                       "\"country\":\"Singapore\",\"admin1\":\"Singapore\"}]}";
    double lat, lon;
    char label[96];
    TEST_ASSERT_TRUE(weather_parse_place(json, &lat, &lon, label, sizeof label));
    TEST_ASSERT_EQUAL_STRING("Singapore", label);
}

static void test_place_not_found(void) {
    double lat, lon;
    char label[96];
    TEST_ASSERT_FALSE(weather_parse_place("{\"generationtime_ms\":0.3}", &lat, &lon, label, sizeof label));
    TEST_ASSERT_FALSE(weather_parse_place("{\"results\":[]}", &lat, &lon, label, sizeof label));
    TEST_ASSERT_FALSE(weather_parse_place("garbage", &lat, &lon, label, sizeof label));
    TEST_ASSERT_FALSE(weather_parse_place(NULL, &lat, &lon, label, sizeof label));
}

static void test_forecast(void) {
    const char *json = "{\"current\":{\"time\":\"2026-10-05T09:00\",\"temperature_2m\":55.4,\"weather_code\":61},"
                       "\"daily\":{\"time\":[\"2026-10-05\",\"2026-10-06\"],\"weather_code\":[61,0],"
                       "\"temperature_2m_max\":[58.1,63.0],\"temperature_2m_min\":[48.2,50.5],"
                       "\"precipitation_probability_max\":[80,null]}}";
    weather_report_t r;
    TEST_ASSERT_TRUE(weather_parse_forecast(json, true, &r));
    TEST_ASSERT_TRUE(r.valid);
    TEST_ASSERT_EQUAL_CHAR('F', r.unit);
    TEST_ASSERT_EQUAL_FLOAT(55.4f, r.temperature);
    TEST_ASSERT_EQUAL_INT(61, r.code);
    TEST_ASSERT_EQUAL_STRING("Light rain", weather_describe(r.code));
    TEST_ASSERT_EQUAL_INT(2, r.day_count);
    TEST_ASSERT_EQUAL_INT(2026, r.days[1].y);
    TEST_ASSERT_EQUAL_INT(6, r.days[1].d);
    TEST_ASSERT_EQUAL_FLOAT(63.0f, r.days[1].high);
    TEST_ASSERT_EQUAL_INT(80, r.days[0].precip_chance);
    TEST_ASSERT_EQUAL_INT(-1, r.days[1].precip_chance);  // null -> unknown
    TEST_ASSERT_EQUAL_STRING("Clear", weather_describe(r.days[1].code));
}

static void test_forecast_bad_input(void) {
    weather_report_t r;
    TEST_ASSERT_FALSE(weather_parse_forecast("{\"error\":true,\"reason\":\"x\"}", false, &r));
    TEST_ASSERT_FALSE(r.valid);
    TEST_ASSERT_FALSE(weather_parse_forecast(NULL, false, &r));
    TEST_ASSERT_EQUAL_STRING("", weather_describe(12345));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_place_found);
    RUN_TEST(test_place_without_region_and_duplicates);
    RUN_TEST(test_place_not_found);
    RUN_TEST(test_forecast);
    RUN_TEST(test_forecast_bad_input);
    return UNITY_END();
}
