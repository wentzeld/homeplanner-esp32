// Over-the-air updates: version order and GitHub's latest-release answer.
#include <stdio.h>
#include <string.h>

#include "unity.h"
#include "update_logic.h"

void setUp(void) {}
void tearDown(void) {}

static void test_versions(void) {
    TEST_ASSERT_TRUE(update_version_cmp("v1.2.10", "1.2.9") > 0);
    TEST_ASSERT_TRUE(update_version_cmp("1.2.9", "1.2.10") < 0);
    TEST_ASSERT_EQUAL_INT(0, update_version_cmp("1.2", "1.2.0"));
    TEST_ASSERT_EQUAL_INT(0, update_version_cmp("v1.1.0", "1.1.0"));
    TEST_ASSERT_TRUE(update_version_cmp("2.0.0", "1.99.99") > 0);
    TEST_ASSERT_TRUE(update_version_cmp("1.1.1", "1.1.0-dirty") > 0);
    TEST_ASSERT_EQUAL_INT(0, update_version_cmp("1.1.0-3-gabc", "1.1.0"));
    TEST_ASSERT_TRUE(update_version_cmp("1.0.0", "") > 0);
    TEST_ASSERT_EQUAL_INT(0, update_version_cmp(NULL, ""));
}

#define RELEASE(extra, assets)                                                                       \
    "{\"tag_name\":\"v1.2.0\",\"name\":\"1.2.0\",\"draft\":false,\"prerelease\":false," extra        \
    "\"body\":\"- Weather icons\\r\\n- Faster sync\\r\\n\",\"assets\":[" assets "]}"
#define ASSET_BIN                                                                                    \
    "{\"name\":\"homeplanner.bin\",\"size\":1832560,\"browser_download_url\":"                       \
    "\"https://github.com/wentzeld/homeplanner-esp32/releases/download/v1.2.0/homeplanner.bin\"}"
#define ASSET_OTHER "{\"name\":\"notes.txt\",\"size\":10,\"browser_download_url\":\"https://example.com/notes.txt\"}"

static void test_release(void) {
    update_release_t r;
    TEST_ASSERT_TRUE(update_parse_release(RELEASE("", ASSET_OTHER "," ASSET_BIN), &r));
    TEST_ASSERT_EQUAL_STRING("1.2.0", r.version);
    TEST_ASSERT_EQUAL_STRING("https://github.com/wentzeld/homeplanner-esp32/releases/download/v1.2.0/homeplanner.bin", r.url);
    TEST_ASSERT_EQUAL_INT(1832560, r.size);
    TEST_ASSERT_EQUAL_STRING("- Weather icons\n- Faster sync", r.notes);
}

static void test_unusable_releases(void) {
    update_release_t r;
    TEST_ASSERT_FALSE(update_parse_release(RELEASE("", ASSET_OTHER), &r));  // no firmware file
    TEST_ASSERT_EQUAL_STRING("", r.version);
    TEST_ASSERT_FALSE(update_parse_release("{\"tag_name\":\"v1.2.0\",\"prerelease\":true,\"assets\":[" ASSET_BIN "]}", &r));
    TEST_ASSERT_FALSE(update_parse_release("{\"tag_name\":\"v1.2.0\",\"draft\":true,\"assets\":[" ASSET_BIN "]}", &r));
    TEST_ASSERT_FALSE(update_parse_release("{\"tag_name\":\"latest\",\"assets\":[" ASSET_BIN "]}", &r));
    TEST_ASSERT_FALSE(update_parse_release("{\"tag_name\":\"v1.2.0\",\"assets\":[{\"name\":\"homeplanner.bin\","
                                           "\"browser_download_url\":\"http://insecure.example/x.bin\"}]}", &r));
    TEST_ASSERT_FALSE(update_parse_release("{\"message\":\"Not Found\"}", &r));  // no releases yet
    TEST_ASSERT_FALSE(update_parse_release("<html>", &r));
    TEST_ASSERT_FALSE(update_parse_release(NULL, &r));
}

static void test_long_notes_are_cut(void) {
    static char json[4096];
    char body[2001];
    memset(body, 'x', 2000);
    body[2000] = '\0';
    snprintf(json, sizeof json, "{\"tag_name\":\"1.3\",\"body\":\"%s\",\"assets\":[" ASSET_BIN "]}", body);
    update_release_t r;
    TEST_ASSERT_TRUE(update_parse_release(json, &r));
    TEST_ASSERT_EQUAL_size_t(sizeof r.notes - 1, strlen(r.notes));
    TEST_ASSERT_EQUAL_STRING("1.3", r.version);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_versions);
    RUN_TEST(test_release);
    RUN_TEST(test_unusable_releases);
    RUN_TEST(test_long_notes_are_cut);
    return UNITY_END();
}
