// "Sign in with Google" for the panel (OAuth 2.0 authorization code + PKCE). Pure C (host-tested):
// building the sign-in address, the token requests, and reading Google's answers.
//
// Flow: the settings page asks the panel for a sign-in address; Google sends the browser back to a
// public https relay page (GitHub Pages), which forwards the code to http://<panel>/oauth/done.
// The `state` carries the panel's address and a single-use nonce; PKCE makes an intercepted code
// useless without the verifier, which never leaves the panel.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#define OAUTH_SCOPES                                              \
    "openid email https://www.googleapis.com/auth/calendar.events " \
    "https://www.googleapis.com/auth/calendar.calendarlist.readonly"
#define OAUTH_AUTH_ENDPOINT "https://accounts.google.com/o/oauth2/v2/auth"
#define OAUTH_TOKEN_ENDPOINT "https://oauth2.googleapis.com/token"
#define OAUTH_REVOKE_ENDPOINT "https://oauth2.googleapis.com/revoke"

#define OAUTH_VERIFIER_LEN 64   // characters (RFC 7636: 43..128)
#define OAUTH_CHALLENGE_LEN 43  // base64url of SHA-256, no padding
#define OAUTH_NONCE_LEN 32      // hex characters

typedef void (*oauth_random_t)(void *buf, size_t len);

// New PKCE pair: a random verifier and its S256 challenge.
void oauth_pkce(char verifier[OAUTH_VERIFIER_LEN + 1], char challenge[OAUTH_CHALLENGE_LEN + 1], oauth_random_t rnd);
// S256 challenge for a verifier (RFC 7636 section 4.2).
void oauth_challenge(const char *verifier, char challenge[OAUTH_CHALLENGE_LEN + 1]);
void oauth_nonce(char nonce[OAUTH_NONCE_LEN + 1], oauth_random_t rnd);

// state = base64url("<host>|<nonce>"). host: the panel's IPv4 address or name (letters, digits, '.',
// '-'; at most 63 characters). False when it doesn't fit or host/nonce are malformed.
bool oauth_state_make(const char *host, const char *nonce, char *out, size_t size);
bool oauth_state_parse(const char *state, char *host, size_t host_size, char *nonce, size_t nonce_size);

// application/x-www-form-urlencoded escaping. Returns false when out is too small.
bool oauth_url_encode(const char *in, char *out, size_t size);

// malloc'd strings (caller frees); NULL when out of memory.
char *oauth_auth_url(const char *client_id, const char *redirect_uri, const char *challenge, const char *state);
char *oauth_code_body(const char *client_id, const char *client_secret, const char *redirect_uri,
                      const char *code, const char *verifier);
char *oauth_refresh_body(const char *client_id, const char *client_secret, const char *refresh_token);

typedef enum {
    OAUTH_OK,
    OAUTH_REVOKED,  // invalid_grant: the sign-in was withdrawn or expired; sign in again
    OAUTH_BAD,      // anything else Google didn't accept, or an unreadable answer
} oauth_result_t;

typedef struct {
    char access_token[2048];
    int expires_in;           // seconds
    char refresh_token[512];  // "" when Google didn't send one (refreshes don't)
    char email[128];          // from the id_token; "" when absent
    char error[64];           // Google's "error" code, e.g. "invalid_grant"
} oauth_tokens_t;

oauth_result_t oauth_parse_token(const char *json, oauth_tokens_t *out);
