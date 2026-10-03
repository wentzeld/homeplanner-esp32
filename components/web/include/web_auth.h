// Signing a computer in to the settings page: a 6-digit one-time code shown on the panel (valid
// 10 minutes, 5 tries), exchanged for a random session token kept in an HttpOnly cookie for 90
// days. Only SHA-256 hashes of tokens are stored. Pure C (host-tested); time and randomness are
// passed in.
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define WEB_CODE_SECONDS (10 * 60)
#define WEB_CODE_TRIES 5
#define WEB_SESSION_SECONDS (90 * 24 * 3600)
#define WEB_MAX_SESSIONS 5
#define WEB_TOKEN_LEN 64  // hex characters
#define WEB_COOKIE "hp_session"

typedef struct {
    uint8_t hash[32];
    int64_t expires;  // 0 = unused
} web_session_t;

typedef struct {
    char code[7];
    int64_t code_expires;  // 0 = no code
    int tries_left;
    web_session_t sessions[WEB_MAX_SESSIONS];
} web_auth_t;

typedef void (*web_random_t)(void *buf, size_t len);

// A new code (replacing any earlier one); returns it.
const char *web_auth_new_code(web_auth_t *a, int64_t now, web_random_t rnd);
void web_auth_cancel_code(web_auth_t *a);
bool web_auth_code_active(const web_auth_t *a, int64_t now);

typedef enum { WEB_LOGIN_OK, WEB_LOGIN_WRONG, WEB_LOGIN_NO_CODE } web_login_t;

// Check a code. OK: the code is used up and token gets a new session (the oldest of 5 is
// replaced). WRONG: one try fewer (none left cancels the code). NO_CODE: none shown, expired,
// or cancelled.
web_login_t web_auth_login(web_auth_t *a, const char *code, int64_t now, web_random_t rnd,
                           char token[WEB_TOKEN_LEN + 1]);

bool web_auth_check(const web_auth_t *a, const char *token, int64_t now);
void web_auth_logout(web_auth_t *a, const char *token);
void web_auth_sign_out_all(web_auth_t *a);
int web_auth_session_count(const web_auth_t *a, int64_t now);

// The session token from a Cookie header ("a=b; hp_session=..."); false if absent/malformed.
bool web_auth_cookie_token(const char *cookie_header, char token[WEB_TOKEN_LEN + 1]);
