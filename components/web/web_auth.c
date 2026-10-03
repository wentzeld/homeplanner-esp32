#include "web_auth.h"

#include <stdio.h>
#include <string.h>

#include "mbedtls/sha256.h"

static void hash_token(const char *token, uint8_t out[32]) {
    mbedtls_sha256((const unsigned char *)token, strlen(token), out, 0);
}

static bool same(const void *a, const void *b, size_t n) {  // constant time
    const uint8_t *x = a, *y = b;
    uint8_t diff = 0;
    for (size_t i = 0; i < n; i++) diff |= x[i] ^ y[i];
    return diff == 0;
}

const char *web_auth_new_code(web_auth_t *a, int64_t now, web_random_t rnd) {
    uint32_t r;
    do {
        rnd(&r, sizeof r);
    } while (r >= 4294000000u);  // 4294000000 = 4294 * 10^6: keeps all codes equally likely
    snprintf(a->code, sizeof a->code, "%06u", (unsigned)(r % 1000000u));
    a->code_expires = now + WEB_CODE_SECONDS;
    a->tries_left = WEB_CODE_TRIES;
    return a->code;
}

void web_auth_cancel_code(web_auth_t *a) {
    memset(a->code, 0, sizeof a->code);
    a->code_expires = 0;
    a->tries_left = 0;
}

bool web_auth_code_active(const web_auth_t *a, int64_t now) {
    return a->code_expires && now < a->code_expires && a->tries_left > 0;
}

web_login_t web_auth_login(web_auth_t *a, const char *code, int64_t now, web_random_t rnd,
                           char token[WEB_TOKEN_LEN + 1]) {
    token[0] = '\0';
    if (!web_auth_code_active(a, now)) {
        web_auth_cancel_code(a);
        return WEB_LOGIN_NO_CODE;
    }
    char given[7] = "";
    size_t n = 0;
    for (const char *p = code ? code : ""; *p && n < 6; p++) {  // "123 456" is fine too
        if (*p >= '0' && *p <= '9') given[n++] = *p;
        else if (*p != ' ' && *p != '-') n = 7;
    }
    if (n != 6 || !same(given, a->code, 6)) {
        if (--a->tries_left <= 0) web_auth_cancel_code(a);
        return WEB_LOGIN_WRONG;
    }
    web_auth_cancel_code(a);

    uint8_t raw[WEB_TOKEN_LEN / 2];
    rnd(raw, sizeof raw);
    for (size_t i = 0; i < sizeof raw; i++) snprintf(token + 2 * i, 3, "%02x", raw[i]);
    web_session_t *slot = &a->sessions[0];
    for (int i = 0; i < WEB_MAX_SESSIONS; i++) {  // a free/expired slot, else the oldest
        web_session_t *s = &a->sessions[i];
        if (s->expires <= now) {
            slot = s;
            break;
        }
        if (s->expires < slot->expires) slot = s;
    }
    hash_token(token, slot->hash);
    slot->expires = now + WEB_SESSION_SECONDS;
    return WEB_LOGIN_OK;
}

static web_session_t *find(const web_auth_t *a, const char *token, int64_t now) {
    if (!token || strlen(token) != WEB_TOKEN_LEN) return NULL;
    uint8_t h[32];
    hash_token(token, h);
    web_session_t *found = NULL;
    for (int i = 0; i < WEB_MAX_SESSIONS; i++) {
        const web_session_t *s = &a->sessions[i];
        if (s->expires > now && same(s->hash, h, sizeof h)) found = (web_session_t *)s;
    }
    return found;
}

bool web_auth_check(const web_auth_t *a, const char *token, int64_t now) { return find(a, token, now) != NULL; }

void web_auth_logout(web_auth_t *a, const char *token) {
    web_session_t *s = find(a, token, INT64_MIN);
    if (s) memset(s, 0, sizeof *s);
}

void web_auth_sign_out_all(web_auth_t *a) { memset(a->sessions, 0, sizeof a->sessions); }

int web_auth_session_count(const web_auth_t *a, int64_t now) {
    int n = 0;
    for (int i = 0; i < WEB_MAX_SESSIONS; i++) n += a->sessions[i].expires > now;
    return n;
}

bool web_auth_cookie_token(const char *cookie_header, char token[WEB_TOKEN_LEN + 1]) {
    token[0] = '\0';
    const size_t name_len = strlen(WEB_COOKIE);
    for (const char *p = cookie_header; p && *p;) {
        while (*p == ' ' || *p == ';') p++;
        if (strncmp(p, WEB_COOKIE, name_len) == 0 && p[name_len] == '=') {
            const char *v = p + name_len + 1;
            size_t n = strcspn(v, "; ");
            if (n != WEB_TOKEN_LEN) return false;
            for (size_t i = 0; i < n; i++) {
                char c = v[i];
                if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
            }
            memcpy(token, v, n);
            token[n] = '\0';
            return true;
        }
        p += strcspn(p, ";");
    }
    return false;
}
