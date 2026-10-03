#include "update.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_crt_bundle.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "net.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "update_logic.h"

static const char *TAG = "update";

#define CONFIRM_US (5LL * 60 * 1000000)  // a new version has 5 minutes to get online
#define CHECK_EVERY_MS (24 * 60 * 60 * 1000)
#define PROJECT "homeplanner"

static SemaphoreHandle_t s_lock;
static update_status_t s_status;
static update_release_t s_release;
static update_listener_t s_listener;
static esp_timer_handle_t s_confirm_timer;
static TaskHandle_t s_check_task;
static bool s_busy;  // an install or upload is running
static char s_notice[200];
static esp_ota_handle_t s_upload;
static const esp_partition_t *s_upload_part;
static size_t s_upload_len;

static SemaphoreHandle_t lock(void) {
    static StaticSemaphore_t storage;
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&storage);
    return s_lock;
}

static void changed(void) {
    if (s_listener) s_listener();
}

static void set_state(update_state_t state, int percent, const char *message) {
    xSemaphoreTake(lock(), portMAX_DELAY);
    s_status.state = state;
    s_status.percent = percent;
    snprintf(s_status.message, sizeof s_status.message, "%s", message ? message : "");
    xSemaphoreGive(lock());
    changed();
}

void update_set_listener(update_listener_t listener) { s_listener = listener; }
const char *update_current_version(void) { return esp_app_get_description()->version; }

update_status_t update_status(void) {
    xSemaphoreTake(lock(), portMAX_DELAY);
    update_status_t s = s_status;
    xSemaphoreGive(lock());
    return s;
}

// --- start-up: confirmation and notices ----------------------------------------------------------

static void nvs_str(const char *key, char *out, size_t size, const char *set) {
    nvs_handle_t h;
    if (nvs_open("homeplanner", set ? NVS_READWRITE : NVS_READONLY, &h) != ESP_OK) {
        if (out) out[0] = '\0';
        return;
    }
    if (set) {
        nvs_set_str(h, key, set);
        nvs_commit(h);
    } else if (nvs_get_str(h, key, out, &size) != ESP_OK) {
        out[0] = '\0';
    }
    nvs_close(h);
}

static void confirm_timeout(void *arg) {
    (void)arg;
    ESP_LOGE(TAG, "this version didn't get online in time: going back to the previous one");
    esp_ota_mark_app_invalid_rollback_and_reboot();
}

void update_boot(void) {
    lock();
    snprintf(s_status.current, sizeof s_status.current, "%s", update_current_version());
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        ESP_LOGW(TAG, "new version %s: it must get online within 5 minutes", s_status.current);
        const esp_timer_create_args_t args = {.callback = confirm_timeout, .name = "ota_confirm"};
        if (esp_timer_create(&args, &s_confirm_timer) == ESP_OK) esp_timer_start_once(s_confirm_timer, CONFIRM_US);
        snprintf(s_notice, sizeof s_notice, "Updated to version %s", s_status.current);
    }
    // A newer version that was undone: say so once.
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    esp_app_desc_t desc;
    if (bad && esp_ota_get_partition_description(bad, &desc) == ESP_OK &&
        update_version_cmp(desc.version, s_status.current) > 0) {
        char seen[32];
        nvs_str("upd_bad_seen", seen, sizeof seen, NULL);
        if (strcmp(seen, desc.version) != 0) {
            snprintf(s_notice, sizeof s_notice, "The update to %.31s didn't work, so the panel went back to %.31s",
                     desc.version, s_status.current);
            nvs_str("upd_bad_seen", NULL, 0, desc.version);
        }
    }
    ESP_LOGI(TAG, "version %s (%s)", s_status.current, running ? running->label : "?");
}

void update_mark_good(void) {
#if CONFIG_HP_OTA_TEST_NEVER_GOOD
    ESP_LOGW(TAG, "test build: not confirming this version (rollback test)");
    return;
#endif
    if (s_confirm_timer) {
        esp_timer_stop(s_confirm_timer);
        esp_timer_delete(s_confirm_timer);
        s_confirm_timer = NULL;
    }
    if (esp_ota_mark_app_valid_cancel_rollback() == ESP_OK) ESP_LOGI(TAG, "version %s confirmed", s_status.current);
}

bool update_take_notice(char *out, size_t size) {
    if (!s_notice[0]) return false;
    snprintf(out, size, "%s", s_notice);
    s_notice[0] = '\0';
    return true;
}

// --- checking GitHub -----------------------------------------------------------------------------

static void check(void) {
    if (s_busy) return;
    set_state(UPDATE_CHECKING, 0, "Checking for updates...");
    char *body = NULL;
    int status = 0;
    esp_err_t e = net_https("GET", "https://api.github.com/repos/" CONFIG_HP_UPDATE_REPO "/releases/latest", NULL, NULL,
                            NULL, &body, &status);
    update_release_t *r = heap_caps_calloc(1, sizeof *r, MALLOC_CAP_SPIRAM);
    if (!r) {
        free(body);
        set_state(UPDATE_FAILED, 0, "Out of memory.");
        return;
    }
    if (e != ESP_OK) {
        set_state(UPDATE_FAILED, 0, "Couldn't reach GitHub to check for updates.");
    } else if (status == 404 || (status == 200 && !update_parse_release(body, r))) {
        xSemaphoreTake(lock(), portMAX_DELAY);
        s_status.checked = time(NULL);
        xSemaphoreGive(lock());
        set_state(UPDATE_UP_TO_DATE, 0, "No updates published yet.");
    } else if (status != 200) {
        char msg[96];
        snprintf(msg, sizeof msg, "GitHub answered with an error (HTTP %d).", status);
        set_state(UPDATE_FAILED, 0, msg);
    } else {
        xSemaphoreTake(lock(), portMAX_DELAY);
        s_status.checked = time(NULL);
        bool newer = update_version_cmp(r->version, s_status.current) > 0;
        if (newer) {
            s_release = *r;
            snprintf(s_status.latest, sizeof s_status.latest, "%s", r->version);
            snprintf(s_status.notes, sizeof s_status.notes, "%s", r->notes);
        }
        xSemaphoreGive(lock());
        if (newer) {
            ESP_LOGI(TAG, "update available: %s", r->version);
            set_state(UPDATE_AVAILABLE, 0, "");
        } else {
            set_state(UPDATE_UP_TO_DATE, 0, "This is the latest version.");
        }
    }
    free(r);
    free(body);
}

static void check_task(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(60 * 1000));
    for (;;) {
        check();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(CHECK_EVERY_MS));
    }
}

void update_start_checks(void) {
    if (!s_check_task) xTaskCreate(check_task, "upd_check", 10 * 1024, NULL, 3, &s_check_task);
}

void update_check_now(void) {
    if (s_check_task) xTaskNotifyGive(s_check_task);
}

// --- installing ----------------------------------------------------------------------------------

static const char *explain(esp_err_t e) {
    switch (e) {
        case ESP_ERR_OTA_VALIDATE_FAILED:
        case ESP_ERR_IMAGE_INVALID: return "The firmware isn't signed with this panel's key, or it's damaged.";
        case ESP_ERR_INVALID_SIZE: return "The firmware is too large.";
        case ESP_ERR_NO_MEM: return "The panel is out of memory.";
        case ESP_ERR_HTTP_CONNECT: return "Couldn't download the update. Check the internet connection.";
        default: return NULL;
    }
}

static void restart_soon(void *arg) {
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
}

static void install_task(void *arg) {
    update_release_t *r = arg;
    char msg[160] = "";
    esp_http_client_config_t http = {
        .url = r->url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .buffer_size = 4096,
        .buffer_size_tx = 2048,  // GitHub's download address after its redirect is long
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = {.http_config = &http};
    esp_https_ota_handle_t ota = NULL;
    esp_err_t e = esp_https_ota_begin(&cfg, &ota);
    if (e == ESP_OK) {
        esp_app_desc_t desc;
        e = esp_https_ota_get_img_desc(ota, &desc);
        if (e == ESP_OK && strcmp(desc.project_name, PROJECT) != 0) {
            snprintf(msg, sizeof msg, "That file isn't HomePlanner firmware.");
            e = ESP_FAIL;
        } else if (e == ESP_OK && update_version_cmp(desc.version, s_status.current) <= 0) {
            snprintf(msg, sizeof msg, "The release contains version %.31s, which isn't newer.", desc.version);
            e = ESP_FAIL;
        }
    }
    int total = ota ? esp_https_ota_get_image_size(ota) : 0, last = -1;
    while (e == ESP_OK) {
        e = esp_https_ota_perform(ota);
        if (e != ESP_ERR_HTTPS_OTA_IN_PROGRESS) break;
        e = ESP_OK;
        int done = esp_https_ota_get_image_len_read(ota);
        int pct = total > 0 ? (int)((long long)done * 100 / total) : 0;
        if (pct != last && pct % 2 == 0) {
            last = pct;
            set_state(UPDATE_INSTALLING, pct, "Downloading the update...");
        }
    }
    if (e == ESP_OK && !esp_https_ota_is_complete_data_received(ota)) e = ESP_FAIL;
    if (e == ESP_OK) {
        set_state(UPDATE_INSTALLING, 100, "Checking the update...");
        e = esp_https_ota_finish(ota);  // verifies the signature and selects the new version
        ota = NULL;
    }
    if (ota) esp_https_ota_abort(ota);
    if (e == ESP_OK) {
        ESP_LOGI(TAG, "installed %s, restarting", r->version);
        set_state(UPDATE_INSTALLING, 100, "Restarting...");
        restart_soon(NULL);
    } else {
        if (!msg[0]) snprintf(msg, sizeof msg, "%s", explain(e) ? explain(e) : "The update didn't install. Please try again.");
        ESP_LOGW(TAG, "update failed: %s (%s)", msg, esp_err_to_name(e));
        s_busy = false;
        set_state(UPDATE_FAILED, 0, msg);
    }
    free(r);
    vTaskDelete(NULL);
}

esp_err_t update_install_available(char *err, size_t err_size) {
    xSemaphoreTake(lock(), portMAX_DELAY);
    bool ok = s_status.state == UPDATE_AVAILABLE && !s_busy && s_release.url[0];
    if (ok) s_busy = true;
    xSemaphoreGive(lock());
    if (!ok) {
        snprintf(err, err_size, s_busy ? "An update is already being installed." : "There's no update to install.");
        return ESP_ERR_INVALID_STATE;
    }
    update_release_t *r = heap_caps_malloc(sizeof *r, MALLOC_CAP_SPIRAM);
    if (r) *r = s_release;
    if (!r || xTaskCreate(install_task, "upd_install", 16 * 1024, r, 5, NULL) != pdPASS) {
        free(r);
        s_busy = false;
        snprintf(err, err_size, "The panel is busy. Please try again.");
        return ESP_ERR_NO_MEM;
    }
    set_state(UPDATE_INSTALLING, 0, "Starting the update...");
    return ESP_OK;
}

// --- uploads -------------------------------------------------------------------------------------

esp_err_t update_upload_begin(size_t size, char *err, size_t err_size) {
    xSemaphoreTake(lock(), portMAX_DELAY);
    bool busy = s_busy;
    s_busy = true;
    xSemaphoreGive(lock());
    if (busy) {
        snprintf(err, err_size, "An update is already being installed.");
        return ESP_ERR_INVALID_STATE;
    }
    s_upload_part = esp_ota_get_next_update_partition(NULL);
    if (!s_upload_part || size > s_upload_part->size) {
        s_busy = false;
        snprintf(err, err_size, s_upload_part ? "The firmware is too large." : "This panel can't update over the air yet (flash it by USB once).");
        return ESP_ERR_INVALID_SIZE;
    }
    esp_err_t e = esp_ota_begin(s_upload_part, size, &s_upload);
    if (e != ESP_OK) {
        s_busy = false;
        snprintf(err, err_size, "Couldn't start the update (%s).", esp_err_to_name(e));
        return e;
    }
    s_upload_len = 0;
    set_state(UPDATE_INSTALLING, 0, "Receiving the update...");
    return ESP_OK;
}

esp_err_t update_upload_write(const void *data, size_t len, char *err, size_t err_size) {
    // The first piece holds the app description: refuse other projects' firmware early.
    size_t desc_at = sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t);
    if (s_upload_len == 0) {
        if (len < desc_at + sizeof(esp_app_desc_t)) {
            snprintf(err, err_size, "The upload is too small to be firmware.");
            return ESP_ERR_INVALID_SIZE;
        }
        const esp_app_desc_t *d = (const esp_app_desc_t *)((const char *)data + desc_at);
        if (d->magic_word != ESP_APP_DESC_MAGIC_WORD || strncmp(d->project_name, PROJECT, sizeof d->project_name) != 0) {
            snprintf(err, err_size, "That file isn't HomePlanner firmware (use build/homeplanner.bin).");
            return ESP_ERR_IMAGE_INVALID;
        }
    }
    esp_err_t e = esp_ota_write(s_upload, data, len);
    if (e != ESP_OK) {
        snprintf(err, err_size, "%s", explain(e) ? explain(e) : "Writing the update failed.");
        return e;
    }
    s_upload_len += len;
    return ESP_OK;
}

esp_err_t update_upload_end(bool complete, char *err, size_t err_size) {
    esp_err_t e = complete ? esp_ota_end(s_upload) : (esp_ota_abort(s_upload), ESP_FAIL);
    if (complete && e == ESP_OK) e = esp_ota_set_boot_partition(s_upload_part);
    if (e != ESP_OK) {
        s_busy = false;
        if (complete) snprintf(err, err_size, "%s", explain(e) ? explain(e) : "The update didn't install.");
        set_state(UPDATE_FAILED, 0, complete ? err : "The upload was interrupted.");
        return e;
    }
    ESP_LOGI(TAG, "uploaded firmware installed (%u bytes), restarting", (unsigned)s_upload_len);
    set_state(UPDATE_INSTALLING, 100, "Restarting...");
    xTaskCreate(restart_soon, "upd_restart", 2048, NULL, 5, NULL);
    return ESP_OK;
}
