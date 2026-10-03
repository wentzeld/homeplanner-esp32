#include "net.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_event.h"
#include "esp_hosted.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "nvs_flash.h"

static const char *TAG = "net";

#define BIT_CONNECTED BIT0
#define BIT_FAILED BIT1

static EventGroupHandle_t s_events;
static bool s_started;
static int s_last_reason;

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        s_last_reason = d->reason;
        xEventGroupClearBits(s_events, BIT_CONNECTED);
        xEventGroupSetBits(s_events, BIT_FAILED);
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        xEventGroupSetBits(s_events, BIT_CONNECTED);
    }
}

esp_err_t net_init(void) {
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_RETURN_ON_ERROR(nvs_flash_erase(), TAG, "nvs erase");
        err = nvs_flash_init();
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs init");
    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    s_events = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&cfg), TAG, "wifi init (C6 link)");
    ESP_RETURN_ON_ERROR(esp_wifi_set_storage(WIFI_STORAGE_RAM), TAG, "wifi storage");
    esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED, on_event, NULL);
    esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL);
    ESP_LOGI(TAG, "Wi-Fi driver ready via ESP32-C6");
    return ESP_OK;
}

esp_err_t net_coprocessor_version(char *out, size_t size) {
    esp_hosted_coprocessor_fwver_t v = {0};
    if (esp_hosted_get_coprocessor_fwversion(&v) != 0) return ESP_FAIL;
    snprintf(out, size, "%lu.%lu.%lu", (unsigned long)v.major1, (unsigned long)v.minor1, (unsigned long)v.patch1);
    return ESP_OK;
}

static esp_err_t ensure_started(wifi_mode_t mode) {
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(mode), TAG, "wifi mode");
    if (!s_started) {
        ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "wifi start");
        s_started = true;
    }
    return ESP_OK;
}

esp_err_t net_start_setup_ap(char ssid_out[33], char pass_out[16]) {
    static bool ap_netif_created;
    if (!ap_netif_created) {
        esp_netif_create_default_wifi_ap();
        ap_netif_created = true;
    }
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BASE);  // the P4 itself has no Wi-Fi MAC; its base MAC is unique per panel
    snprintf(ssid_out, 33, "HomePlanner-%02X%02X", mac[4], mac[5]);
    // 10 characters without look-alikes (0/O, 1/l/I) so it's easy to type if the QR code isn't used.
    static const char alphabet[] = "abcdefghijkmnpqrstuvwxyz23456789";
    for (int i = 0; i < 10; i++) pass_out[i] = alphabet[esp_random() % (sizeof alphabet - 1)];
    pass_out[10] = '\0';

    wifi_config_t ap = {0};
    memcpy(ap.ap.ssid, ssid_out, strlen(ssid_out));
    ap.ap.ssid_len = strlen(ssid_out);
    memcpy(ap.ap.password, pass_out, strlen(pass_out));
    ap.ap.channel = 6;
    ap.ap.max_connection = 2;
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ESP_RETURN_ON_ERROR(ensure_started(WIFI_MODE_APSTA), TAG, "start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &ap), TAG, "ap config");
    ESP_LOGI(TAG, "setup hotspot %s started", ssid_out);
    return ESP_OK;
}

int net_scan(net_ap_t *aps, int max) {
    if (!s_started && ensure_started(WIFI_MODE_STA) != ESP_OK) return 0;
    if (esp_wifi_scan_start(NULL, true) != ESP_OK) return 0;
    uint16_t n = 0;
    esp_wifi_scan_get_ap_num(&n);
    if (n == 0) return 0;
    wifi_ap_record_t *records = calloc(n, sizeof *records);
    if (!records) return 0;
    esp_wifi_scan_get_ap_records(&n, records);  // sorted by signal strength
    int count = 0;
    for (int i = 0; i < n && count < max; i++) {
        const char *ssid = (const char *)records[i].ssid;
        if (!ssid[0]) continue;  // hidden network
        bool dup = false;
        for (int j = 0; j < count && !dup; j++) dup = strcmp(aps[j].ssid, ssid) == 0;
        if (dup) continue;
        snprintf(aps[count].ssid, sizeof aps[count].ssid, "%s", ssid);
        aps[count].rssi = records[i].rssi;
        aps[count].secure = records[i].authmode != WIFI_AUTH_OPEN;
        count++;
    }
    free(records);
    return count;
}

static const char *reason_text(int reason) {
    switch (reason) {
        case WIFI_REASON_NO_AP_FOUND: return "The network wasn't found. Is the router on and in range?";
        case WIFI_REASON_AUTH_FAIL:
        case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_HANDSHAKE_TIMEOUT:
        case WIFI_REASON_MIC_FAILURE: return "Wrong Wi-Fi password.";
        default: return "Couldn't connect to the Wi-Fi network.";
    }
}

esp_err_t net_connect(const char *ssid, const char *pass, int timeout_ms, char *ip_out, size_t ip_size,
                      char *err, size_t err_size) {
    wifi_config_t sta = {0};
    snprintf((char *)sta.sta.ssid, sizeof sta.sta.ssid, "%s", ssid);
    snprintf((char *)sta.sta.password, sizeof sta.sta.password, "%s", pass ? pass : "");
    sta.sta.threshold.authmode = (pass && pass[0]) ? WIFI_AUTH_WPA_PSK : WIFI_AUTH_OPEN;
    sta.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    sta.sta.sort_method = WIFI_CONNECT_AP_BY_SIGNAL;
    ESP_RETURN_ON_ERROR(ensure_started(WIFI_MODE_STA), TAG, "start");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &sta), TAG, "sta config");
    xEventGroupClearBits(s_events, BIT_CONNECTED | BIT_FAILED);
    s_last_reason = 0;
    esp_wifi_connect();
    // A disconnect can be transient; retry within the timeout.
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);
    for (;;) {
        TickType_t now = xTaskGetTickCount();
        if (now >= deadline) break;
        EventBits_t bits = xEventGroupWaitBits(s_events, BIT_CONNECTED | BIT_FAILED, pdTRUE, pdFALSE, deadline - now);
        if (bits & BIT_CONNECTED) {
            xEventGroupSetBits(s_events, BIT_CONNECTED);  // keep "connected" state visible
            esp_netif_ip_info_t ip;
            esp_netif_get_ip_info(esp_netif_get_handle_from_ifkey("WIFI_STA_DEF"), &ip);
            snprintf(ip_out, ip_size, IPSTR, IP2STR(&ip.ip));
            ESP_LOGI(TAG, "connected to %s, IP %s", ssid, ip_out);
            return ESP_OK;
        }
        if (bits & BIT_FAILED) {
            if (s_last_reason == WIFI_REASON_AUTH_FAIL || s_last_reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) break;
            vTaskDelay(pdMS_TO_TICKS(1000));
            esp_wifi_connect();
        }
    }
    esp_wifi_disconnect();
    snprintf(err, err_size, "%s", reason_text(s_last_reason));
    ESP_LOGW(TAG, "connect to %s failed (reason %d)", ssid, s_last_reason);
    return ESP_FAIL;
}

bool net_is_connected(void) { return s_events && (xEventGroupGetBits(s_events) & BIT_CONNECTED); }

esp_err_t net_sync_time(const char *tz_posix, int timeout_ms) {
    setenv("TZ", tz_posix, 1);
    tzset();
    static bool started;
    if (!started) {
        esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
        ESP_RETURN_ON_ERROR(esp_netif_sntp_init(&cfg), TAG, "sntp");
        started = true;
    }
    return esp_netif_sntp_sync_wait(pdMS_TO_TICKS(timeout_ms)) == ESP_OK ? ESP_OK : ESP_ERR_TIMEOUT;
}

#define MAX_RESPONSE (768 * 1024)

esp_err_t net_https(const char *method, const char *url, const char *bearer, const char *content_type,
                    const char *body, char **response, int *status) {
    *response = NULL;
    *status = 0;
    esp_http_client_method_t m = HTTP_METHOD_GET;
    if (strcmp(method, "POST") == 0) m = HTTP_METHOD_POST;
    else if (strcmp(method, "PUT") == 0) m = HTTP_METHOD_PUT;
    else if (strcmp(method, "PATCH") == 0) m = HTTP_METHOD_PATCH;
    else if (strcmp(method, "DELETE") == 0) m = HTTP_METHOD_DELETE;
    esp_http_client_config_t cfg = {
        .url = url,
        .method = m,
        .timeout_ms = 20000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 4096,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) return ESP_ERR_NO_MEM;
    char auth[2200];
    if (bearer) {
        snprintf(auth, sizeof auth, "Bearer %s", bearer);
        esp_http_client_set_header(c, "Authorization", auth);
    }
    if (content_type) esp_http_client_set_header(c, "Content-Type", content_type);
    int body_len = body ? (int)strlen(body) : 0;
    esp_err_t err = esp_http_client_open(c, body_len);
    if (err == ESP_OK && body_len > 0 && esp_http_client_write(c, body, body_len) != body_len) err = ESP_FAIL;
    if (err == ESP_OK && esp_http_client_fetch_headers(c) < 0) err = ESP_FAIL;
    if (err == ESP_OK) {
        *status = esp_http_client_get_status_code(c);
        size_t cap = 8192, len = 0;
        char *buf = malloc(cap);
        while (buf) {
            if (len + 4096 + 1 > cap) {
                if (cap >= MAX_RESPONSE) break;
                char *bigger = realloc(buf, cap * 2);
                if (!bigger) break;
                buf = bigger, cap *= 2;
            }
            int r = esp_http_client_read(c, buf + len, cap - len - 1);
            if (r <= 0) break;
            len += (size_t)r;
        }
        if (buf) buf[len] = '\0';
        *response = buf;
        if (!buf) err = ESP_ERR_NO_MEM;
    }
    if (err != ESP_OK) ESP_LOGW(TAG, "%s %.60s failed: %s", method, url, esp_err_to_name(err));
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return err;
}

esp_err_t net_download(const char *url, size_t max_bytes, char **data, size_t *len, char *err, size_t err_size) {
    *data = NULL;
    *len = 0;
    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 30000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,
        .user_agent = "HomePlanner/1.0",
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (!c) {
        snprintf(err, err_size, "The panel is out of memory.");
        return ESP_ERR_NO_MEM;
    }
    esp_http_client_set_header(c, "Accept", "text/calendar, */*");
    esp_err_t res = ESP_FAIL;
    int status = 0;
    for (int redirects = 0;; redirects++) {
        if (esp_http_client_open(c, 0) != ESP_OK || esp_http_client_fetch_headers(c) < 0) {
            snprintf(err, err_size, "Couldn't reach that address. Check the link and that the panel is online.");
            goto done;
        }
        status = esp_http_client_get_status_code(c);
        bool redirect = status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
        if (!redirect) break;
        if (redirects >= 5) {
            snprintf(err, err_size, "The link redirects too many times.");
            goto done;
        }
        esp_http_client_flush_response(c, NULL);
        if (esp_http_client_set_redirection(c) != ESP_OK) {
            snprintf(err, err_size, "The link redirects to an address the panel can't follow.");
            goto done;
        }
    }
    if (status >= 400 || status < 200) {
        snprintf(err, err_size, "The link returned an error (HTTP %d).%s", status,
                 status == 401 || status == 403 || status == 404
                     ? " Is it the calendar's private iCal link? (Google: Settings and sharing > Secret address in iCal format.)"
                     : "");
        goto done;
    }
    size_t cap = 64 * 1024, n = 0;
    char *buf = heap_caps_malloc(cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    while (buf) {
        if (n + 4096 + 1 > cap) {
            if (cap > max_bytes) {
                snprintf(err, err_size, "That calendar is too large (over %u MB).", (unsigned)(max_bytes >> 20));
                break;
            }
            char *bigger = heap_caps_realloc(buf, cap * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!bigger) {
                snprintf(err, err_size, "The panel is out of memory.");
                break;
            }
            buf = bigger, cap *= 2;
        }
        int r = esp_http_client_read(c, buf + n, cap - n - 1);
        if (r < 0) {
            snprintf(err, err_size, "The download was interrupted. Try again.");
            break;
        }
        if (r == 0) {
            res = ESP_OK;
            break;
        }
        n += (size_t)r;
        if (n > max_bytes) {
            snprintf(err, err_size, "That calendar is too large (over %u MB).", (unsigned)(max_bytes >> 20));
            break;
        }
    }
    if (!buf) snprintf(err, err_size, "The panel is out of memory.");
    if (res == ESP_OK) {
        buf[n] = '\0';
        *data = buf;
        *len = n;
    } else {
        free(buf);
    }
done:
    if (res != ESP_OK) ESP_LOGW(TAG, "download failed (HTTP %d): %s", status, err);
    esp_http_client_close(c);
    esp_http_client_cleanup(c);
    return res;
}

void net_url_encode(const char *in, char *out, size_t size) {
    static const char hex[] = "0123456789ABCDEF";
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *)in; *p && o + 4 < size; p++) {
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("-_.~", *p)) {
            out[o++] = (char)*p;
        } else {
            out[o++] = '%', out[o++] = hex[*p >> 4], out[o++] = hex[*p & 15];
        }
    }
    out[o] = '\0';
}
