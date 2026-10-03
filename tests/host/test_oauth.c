// "Sign in with Google": PKCE, state, sign-in address, token requests and answers.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "oauth.h"
#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static unsigned s_seed = 7;
static void fake_random(void *buf, size_t len) {  // deterministic, not for real use
    unsigned char *b = buf;
    for (size_t i = 0; i < len; i++) {
        s_seed = s_seed * 1103515245u + 12345u;
        b[i] = (unsigned char)(s_seed >> 16);
    }
}

#define NONCE "0123456789abcdef0123456789abcdef"

static void test_pkce_rfc7636_example(void) {
    char challenge[OAUTH_CHALLENGE_LEN + 1];
    oauth_challenge("dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk", challenge);
    TEST_ASSERT_EQUAL_STRING("E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM", challenge);
}

static void test_pkce_pair(void) {
    char v1[OAUTH_VERIFIER_LEN + 1], c1[OAUTH_CHALLENGE_LEN + 1], v2[OAUTH_VERIFIER_LEN + 1], c2[OAUTH_CHALLENGE_LEN + 1];
    oauth_pkce(v1, c1, fake_random);
    oauth_pkce(v2, c2, fake_random);
    TEST_ASSERT_EQUAL_size_t(OAUTH_VERIFIER_LEN, strlen(v1));
    TEST_ASSERT_EQUAL_size_t(OAUTH_CHALLENGE_LEN, strlen(c1));
    for (int i = 0; i < OAUTH_VERIFIER_LEN; i++) TEST_ASSERT_NOT_NULL(strchr("ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~", v1[i]));
    TEST_ASSERT_TRUE(strcmp(v1, v2) != 0);
    char again[OAUTH_CHALLENGE_LEN + 1];
    oauth_challenge(v1, again);
    TEST_ASSERT_EQUAL_STRING(c1, again);
    char nonce[OAUTH_NONCE_LEN + 1];
    oauth_nonce(nonce, fake_random);
    TEST_ASSERT_EQUAL_size_t(OAUTH_NONCE_LEN, strlen(nonce));
}

static void test_state(void) {
    char state[160], host[64], nonce[OAUTH_NONCE_LEN + 1];
    TEST_ASSERT_TRUE(oauth_state_make("192.168.1.50", NONCE, state, sizeof state));
    TEST_ASSERT_EQUAL_STRING("MTkyLjE2OC4xLjUwfDAxMjM0NTY3ODlhYmNkZWYwMTIzNDU2Nzg5YWJjZGVm", state);  // the relay page decodes this
    TEST_ASSERT_TRUE(oauth_state_parse(state, host, sizeof host, nonce, sizeof nonce));
    TEST_ASSERT_EQUAL_STRING("192.168.1.50", host);
    TEST_ASSERT_EQUAL_STRING(NONCE, nonce);
    TEST_ASSERT_TRUE(oauth_state_make("homeplanner.local", NONCE, state, sizeof state));
    TEST_ASSERT_TRUE(oauth_state_parse(state, host, sizeof host, nonce, sizeof nonce));
    TEST_ASSERT_EQUAL_STRING("homeplanner.local", host);

    TEST_ASSERT_FALSE(oauth_state_make("evil.com/x", NONCE, state, sizeof state));
    TEST_ASSERT_FALSE(oauth_state_make("1.2.3.4", "short", state, sizeof state));
    TEST_ASSERT_FALSE(oauth_state_make("1.2.3.4", NONCE, state, 10));
    TEST_ASSERT_FALSE(oauth_state_parse("", host, sizeof host, nonce, sizeof nonce));
    TEST_ASSERT_FALSE(oauth_state_parse("!!!", host, sizeof host, nonce, sizeof nonce));
    TEST_ASSERT_FALSE(oauth_state_parse("MTkyLjE2OC4xLjE5OQ", host, sizeof host, nonce, sizeof nonce));  // no '|'
    TEST_ASSERT_FALSE(oauth_state_parse(NULL, host, sizeof host, nonce, sizeof nonce));
}

static void test_url_encode(void) {
    char out[64];
    TEST_ASSERT_TRUE(oauth_url_encode("a b/c?d=e&f~g.h-i_j", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("a%20b%2Fc%3Fd%3De%26f~g.h-i_j", out);
    TEST_ASSERT_TRUE(oauth_url_encode("\xc3\xa9", out, sizeof out));
    TEST_ASSERT_EQUAL_STRING("%C3%A9", out);
    TEST_ASSERT_FALSE(oauth_url_encode("abc", out, 3));
    TEST_ASSERT_TRUE(oauth_url_encode("abc", out, 4));
    TEST_ASSERT_FALSE(oauth_url_encode("a/", out, 4));
}

static void test_requests(void) {
    char *url = oauth_auth_url("id.apps.googleusercontent.com", "https://example.github.io/x/oauth.html", "CHAL", "STATE");
    TEST_ASSERT_NOT_NULL(url);
    TEST_ASSERT_EQUAL_STRING_LEN(OAUTH_AUTH_ENDPOINT "?client_id=id.apps.googleusercontent.com&", url, strlen(OAUTH_AUTH_ENDPOINT) + 41);
    TEST_ASSERT_NOT_NULL(strstr(url, "&redirect_uri=https%3A%2F%2Fexample.github.io%2Fx%2Foauth.html&"));
    TEST_ASSERT_NOT_NULL(strstr(url, "&response_type=code&"));
    TEST_ASSERT_NOT_NULL(strstr(url, "&scope=openid%20email%20https%3A%2F%2Fwww.googleapis.com%2Fauth%2Fcalendar.events%20"));
    TEST_ASSERT_NOT_NULL(strstr(url, "calendar.calendarlist.readonly&"));
    TEST_ASSERT_NOT_NULL(strstr(url, "&code_challenge=CHAL&code_challenge_method=S256&state=STATE&"));
    TEST_ASSERT_NOT_NULL(strstr(url, "&access_type=offline&prompt=consent"));
    free(url);

    char *body = oauth_code_body("cid", "sec ret", "https://r", "4/0Ab+c", "verif");
    TEST_ASSERT_EQUAL_STRING("grant_type=authorization_code&code=4%2F0Ab%2Bc&code_verifier=verif&client_id=cid&"
                             "client_secret=sec%20ret&redirect_uri=https%3A%2F%2Fr", body);
    free(body);
    body = oauth_refresh_body("cid", "sec", "1//0g-x");
    TEST_ASSERT_EQUAL_STRING("grant_type=refresh_token&refresh_token=1%2F%2F0g-x&client_id=cid&client_secret=sec", body);
    free(body);
}

static void test_token_answers(void) {
    static oauth_tokens_t t;
    TEST_ASSERT_EQUAL_INT(OAUTH_OK, oauth_parse_token(
        "{\"access_token\":\"ya29.abc\",\"expires_in\":3599,\"refresh_token\":\"1//0g\",\"scope\":\"x\","
        "\"token_type\":\"Bearer\",\"id_token\":\"eyJhbGciOiJSUzI1NiJ9.eyJpc3MiOiJodHRwczovL2FjY291bnRzLmdvb2dsZS5jb20iLCJlbWFpbCI6"
        "InBhcmVudEBleGFtcGxlLmNvbSIsImVtYWlsX3ZlcmlmaWVkIjp0cnVlfQ.c2ln\"}", &t));
    TEST_ASSERT_EQUAL_STRING("ya29.abc", t.access_token);
    TEST_ASSERT_EQUAL_INT(3599, t.expires_in);
    TEST_ASSERT_EQUAL_STRING("1//0g", t.refresh_token);
    TEST_ASSERT_EQUAL_STRING("parent@example.com", t.email);

    // A refresh answer has no refresh_token or id_token.
    TEST_ASSERT_EQUAL_INT(OAUTH_OK, oauth_parse_token("{\"access_token\":\"ya29.def\",\"expires_in\":3600}", &t));
    TEST_ASSERT_EQUAL_STRING("", t.refresh_token);
    TEST_ASSERT_EQUAL_STRING("", t.email);

    TEST_ASSERT_EQUAL_INT(OAUTH_REVOKED, oauth_parse_token("{\"error\":\"invalid_grant\",\"error_description\":\"Token has been expired or revoked.\"}", &t));
    TEST_ASSERT_EQUAL_INT(OAUTH_BAD, oauth_parse_token("{\"error\":\"invalid_client\"}", &t));
    TEST_ASSERT_EQUAL_STRING("invalid_client", t.error);
    TEST_ASSERT_EQUAL_INT(OAUTH_BAD, oauth_parse_token("<html>", &t));
    TEST_ASSERT_EQUAL_INT(OAUTH_BAD, oauth_parse_token(NULL, &t));
    TEST_ASSERT_EQUAL_INT(OAUTH_OK, oauth_parse_token("{\"access_token\":\"a\",\"id_token\":\"garbage\"}", &t));
    TEST_ASSERT_EQUAL_STRING("", t.email);
}

int main(void) {
    UNITY_BEGIN();
    RUN_TEST(test_pkce_rfc7636_example);
    RUN_TEST(test_pkce_pair);
    RUN_TEST(test_state);
    RUN_TEST(test_url_encode);
    RUN_TEST(test_requests);
    RUN_TEST(test_token_answers);
    return UNITY_END();
}
