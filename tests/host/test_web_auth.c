// One-time code and session cookies for the settings page.
#include <stdlib.h>
#include <string.h>

#include "unity.h"
#include "web_auth.h"

void setUp(void) {}
void tearDown(void) {}

static uint32_t s_seed = 1;
static void fake_random(void *buf, size_t len) {  // deterministic, not for real use
    uint8_t *b = buf;
    for (size_t i = 0; i < len; i++) {
        s_seed = s_seed * 1103515245u + 12345u;
        b[i] = (uint8_t)(s_seed >> 16);
    }
}

#define NOW 1790000000LL

static void test_code_format_and_expiry(void) {
    web_auth_t a = {0};
    TEST_ASSERT_FALSE(web_auth_code_active(&a, NOW));
    const char *code = web_auth_new_code(&a, NOW, fake_random);
    TEST_ASSERT_EQUAL_size_t(6, strlen(code));
    for (int i = 0; i < 6; i++) TEST_ASSERT_TRUE(code[i] >= '0' && code[i] <= '9');
    TEST_ASSERT_TRUE(web_auth_code_active(&a, NOW + WEB_CODE_SECONDS - 1));
    TEST_ASSERT_FALSE(web_auth_code_active(&a, NOW + WEB_CODE_SECONDS));
    char token[WEB_TOKEN_LEN + 1];
    char copy[7];
    strcpy(copy, code);
    TEST_ASSERT_EQUAL_INT(WEB_LOGIN_NO_CODE, web_auth_login(&a, copy, NOW + WEB_CODE_SECONDS, fake_random, token));
    TEST_ASSERT_EQUAL_STRING("", token);
}

static void test_login_once_then_session(void) {
    web_auth_t a = {0};
    char code[7], token[WEB_TOKEN_LEN + 1], token2[WEB_TOKEN_LEN + 1];
    strcpy(code, web_auth_new_code(&a, NOW, fake_random));
    char spaced[10] = {code[0], code[1], code[2], ' ', code[3], code[4], code[5], 0};
    TEST_ASSERT_EQUAL_INT(WEB_LOGIN_OK, web_auth_login(&a, spaced, NOW + 5, fake_random, token));
    TEST_ASSERT_EQUAL_size_t(WEB_TOKEN_LEN, strlen(token));
    TEST_ASSERT_TRUE(web_auth_check(&a, token, NOW + 10));
    TEST_ASSERT_TRUE(web_auth_check(&a, token, NOW + 5 + WEB_SESSION_SECONDS - 1));
    TEST_ASSERT_FALSE(web_auth_check(&a, token, NOW + 5 + WEB_SESSION_SECONDS));
    // One-time: the same code doesn't work again.
    TEST_ASSERT_EQUAL_INT(WEB_LOGIN_NO_CODE, web_auth_login(&a, code, NOW + 6, fake_random, token2));
    // Only hashes are stored.
    TEST_ASSERT_NULL(memmem(&a, sizeof a, token, WEB_TOKEN_LEN));
    TEST_ASSERT_FALSE(web_auth_check(&a, "", NOW));
    TEST_ASSERT_FALSE(web_auth_check(&a, NULL, NOW));
    char other[WEB_TOKEN_LEN + 1];
    memset(other, 'a', WEB_TOKEN_LEN);
    other[WEB_TOKEN_LEN] = 0;
    TEST_ASSERT_FALSE(web_auth_check(&a, other, NOW));
    web_auth_logout(&a, token);
    TEST_ASSERT_FALSE(web_auth_check(&a, token, NOW + 10));
}

static void test_five_wrong_tries_cancel_the_code(void) {
    web_auth_t a = {0};
    char code[7], wrong[7], token[WEB_TOKEN_LEN + 1];
    strcpy(code, web_auth_new_code(&a, NOW, fake_random));
    strcpy(wrong, code);
    wrong[0] = wrong[0] == '9' ? '0' : wrong[0] + 1;
    for (int i = 0; i < WEB_CODE_TRIES; i++) {
        TEST_ASSERT_EQUAL_INT(WEB_LOGIN_WRONG, web_auth_login(&a, wrong, NOW + 1, fake_random, token));
    }
    TEST_ASSERT_FALSE(web_auth_code_active(&a, NOW + 1));
    TEST_ASSERT_EQUAL_INT(WEB_LOGIN_NO_CODE, web_auth_login(&a, code, NOW + 1, fake_random, token));
    TEST_ASSERT_EQUAL_INT(0, web_auth_session_count(&a, NOW));
    // Malformed input counts as a wrong try.
    strcpy(code, web_auth_new_code(&a, NOW, fake_random));
    TEST_ASSERT_EQUAL_INT(WEB_LOGIN_WRONG, web_auth_login(&a, "12345", NOW, fake_random, token));
    TEST_ASSERT_EQUAL_INT(WEB_LOGIN_WRONG, web_auth_login(&a, "1234567", NOW, fake_random, token));
    TEST_ASSERT_EQUAL_INT(WEB_LOGIN_WRONG, web_auth_login(&a, "abcdef", NOW, fake_random, token));
    TEST_ASSERT_EQUAL_INT(WEB_CODE_TRIES - 3, a.tries_left);
    TEST_ASSERT_EQUAL_INT(WEB_LOGIN_OK, web_auth_login(&a, code, NOW, fake_random, token));
}

static void test_at_most_five_devices_and_sign_out_all(void) {
    web_auth_t a = {0};
    char tokens[7][WEB_TOKEN_LEN + 1];
    for (int i = 0; i < 7; i++) {
        char code[7];
        strcpy(code, web_auth_new_code(&a, NOW + i, fake_random));
        TEST_ASSERT_EQUAL_INT(WEB_LOGIN_OK, web_auth_login(&a, code, NOW + i, fake_random, tokens[i]));
    }
    TEST_ASSERT_EQUAL_INT(WEB_MAX_SESSIONS, web_auth_session_count(&a, NOW + 10));
    TEST_ASSERT_FALSE(web_auth_check(&a, tokens[0], NOW + 10));  // the oldest two were replaced
    TEST_ASSERT_FALSE(web_auth_check(&a, tokens[1], NOW + 10));
    for (int i = 2; i < 7; i++) TEST_ASSERT_TRUE(web_auth_check(&a, tokens[i], NOW + 10));
    web_auth_sign_out_all(&a);
    for (int i = 0; i < 7; i++) TEST_ASSERT_FALSE(web_auth_check(&a, tokens[i], NOW + 10));
    TEST_ASSERT_EQUAL_INT(0, web_auth_session_count(&a, NOW + 10));
}

static void test_cookie_header(void) {
    char token[WEB_TOKEN_LEN + 1], good[WEB_TOKEN_LEN + 1], header[200];
    for (int i = 0; i < WEB_TOKEN_LEN; i++) good[i] = "0123456789abcdef"[i % 16];
    good[WEB_TOKEN_LEN] = 0;
    snprintf(header, sizeof header, "theme=dark; " WEB_COOKIE "=%s; other=1", good);
    TEST_ASSERT_TRUE(web_auth_cookie_token(header, token));
    TEST_ASSERT_EQUAL_STRING(good, token);
    snprintf(header, sizeof header, WEB_COOKIE "=%s", good);
    TEST_ASSERT_TRUE(web_auth_cookie_token(header, token));
    TEST_ASSERT_FALSE(web_auth_cookie_token("x" WEB_COOKIE "=abc", token));
    TEST_ASSERT_FALSE(web_auth_cookie_token(WEB_COOKIE "=short", token));
    snprintf(header, sizeof header, WEB_COOKIE "=%.63sZ", good);
    TEST_ASSERT_FALSE(web_auth_cookie_token(header, token));
    TEST_ASSERT_FALSE(web_auth_cookie_token(NULL, token));
    TEST_ASSERT_FALSE(web_auth_cookie_token("", token));
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_code_format_and_expiry);
    RUN_TEST(test_login_once_then_session);
    RUN_TEST(test_five_wrong_tries_cancel_the_code);
    RUN_TEST(test_at_most_five_devices_and_sign_out_all);
    RUN_TEST(test_cookie_header);
    return UNITY_END();
}
