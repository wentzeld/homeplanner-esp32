#include "oauth.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "mbedtls/base64.h"
#include "mbedtls/sha256.h"

static void base64url(const unsigned char *in, size_t len, char *out, size_t size) {
    size_t n = 0;
    out[0] = '\0';
    if (mbedtls_base64_encode((unsigned char *)out, size, &n, in, len) != 0) return;
    while (n && out[n - 1] == '=') n--;
    out[n] = '\0';
    for (size_t i = 0; i < n; i++) {
        if (out[i] == '+') out[i] = '-';
        else if (out[i] == '/') out[i] = '_';
    }
}

// base64url (no padding) -> bytes; returns the length or -1.
static int unbase64url(const char *in, size_t in_len, unsigned char *out, size_t size) {
    size_t padded = in_len + (4 - in_len % 4) % 4;
    char *tmp = malloc(padded + 1);
    if (!tmp) return -1;
    for (size_t i = 0; i < in_len; i++) {
        char c = in[i];
        tmp[i] = c == '-' ? '+' : c == '_' ? '/' : c;
    }
    for (size_t i = in_len; i < padded; i++) tmp[i] = '=';
    tmp[padded] = '\0';
    size_t n = 0;
    int r = mbedtls_base64_decode(out, size, &n, (unsigned char *)tmp, padded);
    free(tmp);
    return r == 0 ? (int)n : -1;
}

void oauth_challenge(const char *verifier, char challenge[OAUTH_CHALLENGE_LEN + 1]) {
    unsigned char hash[32];
    mbedtls_sha256((const unsigned char *)verifier, strlen(verifier), hash, 0);
    char buf[48];
    base64url(hash, sizeof hash, buf, sizeof buf);  // always 43 characters
    memcpy(challenge, buf, OAUTH_CHALLENGE_LEN);
    challenge[OAUTH_CHALLENGE_LEN] = '\0';
}

void oauth_pkce(char verifier[OAUTH_VERIFIER_LEN + 1], char challenge[OAUTH_CHALLENGE_LEN + 1], oauth_random_t rnd) {
    static const char ALPHABET[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-._~";  // 66
    for (int i = 0; i < OAUTH_VERIFIER_LEN; i++) {
        unsigned char b;
        do {
            rnd(&b, 1);
        } while (b >= 198);  // 198 = 3 * 66: every character equally likely
        verifier[i] = ALPHABET[b % 66];
    }
    verifier[OAUTH_VERIFIER_LEN] = '\0';
    oauth_challenge(verifier, challenge);
}

void oauth_nonce(char nonce[OAUTH_NONCE_LEN + 1], oauth_random_t rnd) {
    unsigned char raw[OAUTH_NONCE_LEN / 2];
    rnd(raw, sizeof raw);
    for (size_t i = 0; i < sizeof raw; i++) snprintf(nonce + 2 * i, 3, "%02x", raw[i]);
}

static bool host_ok(const char *host) {
    size_t n = strlen(host);
    if (n == 0 || n > 63) return false;
    for (size_t i = 0; i < n; i++) {
        char c = host[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '-')) return false;
    }
    return true;
}

static bool nonce_ok(const char *nonce) {
    if (strlen(nonce) != OAUTH_NONCE_LEN) return false;
    for (int i = 0; i < OAUTH_NONCE_LEN; i++) {
        char c = nonce[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    }
    return true;
}

bool oauth_state_make(const char *host, const char *nonce, char *out, size_t size) {
    if (!host || !nonce || !host_ok(host) || !nonce_ok(nonce)) return false;
    char plain[100];
    int n = snprintf(plain, sizeof plain, "%s|%s", host, nonce);
    char buf[140];
    base64url((const unsigned char *)plain, (size_t)n, buf, sizeof buf);
    if (!buf[0] || strlen(buf) >= size) return false;
    snprintf(out, size, "%s", buf);
    return true;
}

bool oauth_state_parse(const char *state, char *host, size_t host_size, char *nonce, size_t nonce_size) {
    if (!state || strlen(state) > 136) return false;
    unsigned char plain[110];
    int n = unbase64url(state, strlen(state), plain, sizeof plain - 1);
    if (n <= 0) return false;
    plain[n] = '\0';
    char *bar = strchr((char *)plain, '|');
    if (!bar) return false;
    *bar = '\0';
    const char *h = (const char *)plain, *nn = bar + 1;
    if (!host_ok(h) || !nonce_ok(nn) || strlen(h) >= host_size || strlen(nn) >= nonce_size) return false;
    snprintf(host, host_size, "%s", h);
    snprintf(nonce, nonce_size, "%s", nn);
    return true;
}

bool oauth_url_encode(const char *in, char *out, size_t size) {
    static const char HEX[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p; p++) {
        bool plain = (*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p);
        if (o + (plain ? 1 : 3) >= size) return false;
        if (plain) {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%';
            out[o++] = HEX[*p >> 4];
            out[o++] = HEX[*p & 15];
        }
    }
    if (o >= size) return false;
    out[o] = '\0';
    return true;
}

// "k1=v1&k2=v2..." from pairs (values encoded). malloc'd.
static char *form(const char *const *pairs, int count) {
    size_t cap = 1;
    for (int i = 0; i < count; i++) cap += strlen(pairs[2 * i]) + 3 * strlen(pairs[2 * i + 1]) + 2;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = 0;
    for (int i = 0; i < count; i++) {
        o += (size_t)snprintf(out + o, cap - o, "%s%s=", i ? "&" : "", pairs[2 * i]);
        if (!oauth_url_encode(pairs[2 * i + 1], out + o, cap - o)) {
            free(out);
            return NULL;
        }
        o += strlen(out + o);
    }
    return out;
}

char *oauth_auth_url(const char *client_id, const char *redirect_uri, const char *challenge, const char *state) {
    const char *pairs[] = {
        "client_id", client_id, "redirect_uri", redirect_uri, "response_type", "code",
        "scope", OAUTH_SCOPES, "code_challenge", challenge, "code_challenge_method", "S256",
        "state", state, "access_type", "offline", "prompt", "consent",
    };
    char *query = form(pairs, 9);
    if (!query) return NULL;
    size_t len = strlen(OAUTH_AUTH_ENDPOINT) + 1 + strlen(query) + 1;
    char *url = malloc(len);
    if (url) snprintf(url, len, "%s?%s", OAUTH_AUTH_ENDPOINT, query);
    free(query);
    return url;
}

char *oauth_code_body(const char *client_id, const char *client_secret, const char *redirect_uri,
                      const char *code, const char *verifier) {
    const char *pairs[] = {
        "grant_type", "authorization_code", "code", code, "code_verifier", verifier,
        "client_id", client_id, "client_secret", client_secret, "redirect_uri", redirect_uri,
    };
    return form(pairs, 6);
}

char *oauth_refresh_body(const char *client_id, const char *client_secret, const char *refresh_token) {
    const char *pairs[] = {
        "grant_type", "refresh_token", "refresh_token", refresh_token,
        "client_id", client_id, "client_secret", client_secret,
    };
    return form(pairs, 4);
}

static void copy_json_str(const cJSON *root, const char *key, char *out, size_t size) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(v) && strlen(v->valuestring) < size) snprintf(out, size, "%s", v->valuestring);
}

// The "email" claim of an id_token (header.payload.signature). It arrived from Google over TLS,
// so the signature isn't checked; it's only used to show who is signed in.
static void id_token_email(const char *jwt, char *out, size_t size) {
    const char *dot1 = strchr(jwt, '.');
    const char *dot2 = dot1 ? strchr(dot1 + 1, '.') : NULL;
    if (!dot2) return;
    size_t len = (size_t)(dot2 - dot1 - 1);
    unsigned char *payload = malloc(len + 4);
    if (!payload) return;
    int n = unbase64url(dot1 + 1, len, payload, len + 3);
    if (n > 0) {
        payload[n] = '\0';
        cJSON *claims = cJSON_Parse((const char *)payload);
        copy_json_str(claims, "email", out, size);
        cJSON_Delete(claims);
    }
    free(payload);
}

oauth_result_t oauth_parse_token(const char *json, oauth_tokens_t *out) {
    memset(out, 0, sizeof *out);
    cJSON *root = cJSON_Parse(json ? json : "");
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return OAUTH_BAD;
    }
    copy_json_str(root, "error", out->error, sizeof out->error);
    copy_json_str(root, "access_token", out->access_token, sizeof out->access_token);
    copy_json_str(root, "refresh_token", out->refresh_token, sizeof out->refresh_token);
    const cJSON *exp = cJSON_GetObjectItemCaseSensitive(root, "expires_in");
    out->expires_in = cJSON_IsNumber(exp) ? exp->valueint : 3600;
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(root, "id_token");
    if (cJSON_IsString(id)) id_token_email(id->valuestring, out->email, sizeof out->email);
    cJSON_Delete(root);
    if (strcmp(out->error, "invalid_grant") == 0) return OAUTH_REVOKED;
    return out->access_token[0] && !out->error[0] ? OAUTH_OK : OAUTH_BAD;
}
