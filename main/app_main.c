// HomePlanner firmware entry point: setup mode (hotspot + settings page) or run mode.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_system.h"
#include "gauth.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "model.h"
#include "net.h"
#include "settings.h"
#include "ui.h"
#include "update.h"
#include "weather.h"
#include "web.h"

static const char *TAG = "main";
static TaskHandle_t s_main_task;  // runs supervise()

#define UI(call)                    \
    do {                            \
        if (lvgl_port_lock(0)) {    \
            call;                   \
            lvgl_port_unlock();     \
        }                           \
    } while (0)

static void on_model_change(void) { UI(ui_week_update()); }

static void restart_into_setup(void) {
    settings_request_setup(true);
    esp_restart();
}

static void settings_saved(void) { esp_restart(); }

static void computer_signed_in(void) { UI(ui_web_signed_in()); }

static void update_changed(void) { UI(ui_update_changed()); }

static void update_screen(bool on) { board_backlight_hold(!on); }

static void google_changed(void) {
    if (s_main_task) xTaskNotifyGive(s_main_task);
}

static void calendar_added(const hp_calendar_t *cal, char *ics, size_t len) { model_calendar_added(cal, ics, len); }

static void calendars_changed(void) { model_calendars_changed(); }

static bool calendar_status(const char *id, web_calendar_status_t *out) {
    return model_calendar_status(id, &out->last_updated, out->error, sizeof out->error);
}

static const web_hooks_t WEB_HOOKS = {
    .settings_saved = settings_saved,
    .calendar_added = calendar_added,
    .calendars_changed = calendars_changed,
    .calendar_status = calendar_status,
    .signed_in = computer_signed_in,
    .google_changed = google_changed,
};

static void run_setup_mode(void) {
    char ssid[33], pass[16];
    if (net_start_setup_ap(ssid, pass) != ESP_OK || web_start_setup(&WEB_HOOKS) != ESP_OK) {
        UI(ui_show_error("Setup couldn't start", "The panel couldn't start its setup hotspot. Restart it and try again."));
        return;
    }
    update_mark_good();  // the setup page is up: this version works
    UI(ui_show_setup(ssid, pass));
}

// Calendar when setup is done and Google is connected; otherwise the "finish setup" QR screen
// (or "sign in again" when Google withdrew access while running). Woken by the web hooks.

static void supervise(hp_settings_t *s) {
    bool calendar = false, qr = false;
    for (;;) {
        bool ready = s->complete && gauth_signed_in();
        if (ready && qr) {
            UI(ui_close_qr());
            qr = false;
            model_refresh_now();
        }
        if (ready && !calendar) {
            model_start(s, on_model_change);  // before the screen: it reads the model (cached copy first)
            UI(ui_week_show(s));
            calendar = true;
            char notice[200];
            if (update_take_notice(notice, sizeof notice)) UI(ui_notice(notice));  // "Updated to 1.2.0"
        }
        if (!ready && !qr) {
            ESP_LOGI(TAG, "%s", calendar ? "Google sign-in expired" : "setup not finished: showing the QR");
            UI(ui_show_qr(calendar ? UI_QR_EXPIRED : UI_QR_FINISH));
            qr = true;
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(3000));
    }
}

static void run_mode(hp_settings_t *s) {
    char ip[16], err[96], detail[256];
    for (;;) {
        snprintf(detail, sizeof detail, "Joining  %s ...", s->wifi_ssid);
        UI(ui_show_status("Connecting", detail));
        if (net_connect(s->wifi_ssid, s->wifi_pass, 30000, ip, sizeof ip, err, sizeof err) == ESP_OK) break;
        snprintf(detail, sizeof detail, "%s\n\nTrying again in 30 seconds. To fix the network name or password, tap Change settings.", err);
        char title[64];
        snprintf(title, sizeof title, "Can't join %s", s->wifi_ssid);
        UI(ui_show_error(title, detail));
        vTaskDelay(pdMS_TO_TICKS(30000));
    }
    bool synced = net_sync_time(s->tz_posix, 15000) == ESP_OK;
    ESP_LOGI(TAG, "online: %s, time %s", ip, synced ? "synced" : "NOT synced");

    // A city/ZIP from the setup page is looked up once, now that the panel is online.
    char place[128];
    if (!s->has_coords && s->place[0]) {
        hp_settings_t updated = *s;
        esp_err_t err = weather_find_place(s->place, &updated.latitude, &updated.longitude, place, sizeof place);
        if (err == ESP_OK) {
            updated.has_coords = true;
            settings_save(&updated);
            *s = updated;
        } else if (err == ESP_ERR_NOT_FOUND) {
            snprintf(place, sizeof place, "\"%s\" not found - try a ZIP code (Change settings)", s->place);
        } else {
            snprintf(place, sizeof place, "\"%s\" (couldn't look it up yet)", s->place);
        }
    } else if (s->place[0]) {
        snprintf(place, sizeof place, "%s", s->place);
    } else {
        snprintf(place, sizeof place, "%.2f, %.2f", s->latitude, s->longitude);
    }
    ESP_LOGI(TAG, "weather for %s", place);
    if (!synced) {  // the calendar needs the real date; keep trying in the background
        UI(ui_show_connected(s->wifi_ssid, ip, s->cal.timezone, false, place));
        while (net_sync_time(s->tz_posix, 15000) != ESP_OK) vTaskDelay(pdMS_TO_TICKS(15000));
    }
    // The settings page for phones/computers (after the clock: sign-ins expire after 90 days).
    char refresh[512], email[128];
    if (settings_load_google(refresh, sizeof refresh, email, sizeof email)) gauth_init(refresh, email);
    if (web_start_run(&WEB_HOOKS, ip) == ESP_OK) {
        UI(ui_set_web_address(ip));
        update_mark_good();  // online with the settings page up: this version works
    }
    update_start_checks();
    supervise(s);
}

void app_main(void) {
    update_boot();  // first: a just-installed version must prove itself (see update.h)
    update_set_listener(update_changed);
    update_set_screen_hook(update_screen);
    ESP_ERROR_CHECK(board_init());
    ui_set_change_settings_cb(restart_into_setup);
    UI(ui_show_status("HomePlanner", "Starting..."));
    board_set_backlight(80);

    if (net_init() != ESP_OK) {
        UI(ui_show_error("Wi-Fi chip not responding",
                         "The panel's Wi-Fi chip (ESP32-C6) didn't start. Unplug the panel for 10 seconds and plug it back in."));
        return;
    }
    static hp_settings_t settings;
    s_main_task = xTaskGetCurrentTaskHandle();
    bool configured = settings_load(&settings);  // at least Wi-Fi
    if (!configured || settings_setup_requested()) {
        ESP_LOGI(TAG, "setup mode (configured=%d requested=%d)", configured, settings_setup_requested());
        run_setup_mode();
    } else {
        run_mode(&settings);
    }
}
