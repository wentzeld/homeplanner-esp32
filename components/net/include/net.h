// Networking: Wi-Fi via the ESP32-C6 co-processor (esp_hosted + esp_wifi_remote), the setup
// hotspot, and clock sync.
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

typedef struct {
    char ssid[33];
    int rssi;
    bool secure;
} net_ap_t;

// NVS, netif, event loop and the Wi-Fi driver (via the C6). Call once; Wi-Fi is not started yet.
esp_err_t net_init(void);

// C6 firmware version, e.g. "2.12.3". Requires net_init().
esp_err_t net_coprocessor_version(char *out, size_t size);

// Start the setup hotspot "HomePlanner-XXXX" with a random WPA2 password (station stays on
// for scanning). Writes the name and password for showing on screen.
esp_err_t net_start_setup_ap(char ssid_out[33], char pass_out[16]);

// Blocking scan; returns the number of networks written to aps (strongest first, deduplicated).
int net_scan(net_ap_t *aps, int max);

// Join a network (station mode). On failure, err gets a user-facing reason.
esp_err_t net_connect(const char *ssid, const char *pass, int timeout_ms, char *ip_out, size_t ip_size,
                      char *err, size_t err_size);
bool net_is_connected(void);

// HTTPS request (certificate-checked). body/content_type may be NULL; bearer adds
// "Authorization: Bearer ...". The response body is malloc'd (caller frees) and
// NUL-terminated; *status gets the HTTP status. Returns ESP_OK if a response was received.
esp_err_t net_https(const char *method, const char *url, const char *bearer, const char *content_type,
                    const char *body, char **response, int *status);

// GET a file of up to max_bytes (http or https, redirects followed) into PSRAM. *data is
// NUL-terminated; caller frees. On failure err gets a user-facing reason.
esp_err_t net_download(const char *url, size_t max_bytes, char **data, size_t *len, char *err, size_t err_size);

// URL-encode a query value.
void net_url_encode(const char *in, char *out, size_t size);

// Set the local timezone (POSIX TZ string) and sync the clock over NTP.
esp_err_t net_sync_time(const char *tz_posix, int timeout_ms);
