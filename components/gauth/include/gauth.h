// The panel's Google sign-in on the device: the stored refresh token is exchanged for short-lived
// access tokens. The pure parts (sign-in address, requests, answers) are in oauth.h.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

// Whether this build has a Google OAuth client configured (secrets.defaults).
bool gauth_configured(void);

// Use this refresh token (NULL/"" = not signed in). email is shown on the settings page.
void gauth_init(const char *refresh_token, const char *email);
bool gauth_signed_in(void);
const char *gauth_email(void);  // "" when unknown
// The sign-in was withdrawn or expired (Google answered invalid_grant): sign in again.
bool gauth_revoked(void);

// A valid access token (cached, refreshed ~5 min before expiry). err gets a user-facing reason;
// ESP_ERR_TIMEOUT when Google can't be reached.
esp_err_t gauth_token(char *token, size_t size, char *err, size_t err_size);

// Finish a sign-in: exchange the code (with the PKCE verifier) for tokens, save them, and start
// using them. err gets a user-facing reason.
esp_err_t gauth_exchange_code(const char *code, const char *verifier, char *err, size_t err_size);

// Sign out: tell Google to withdraw the sign-in (best effort) and forget it on the panel.
void gauth_sign_out(void);
