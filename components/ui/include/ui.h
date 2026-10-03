// HomePlanner screens (LVGL). Call only with the LVGL lock held (lvgl_port_lock).
#pragma once
#include <stdbool.h>

#include "lvgl.h"
#include "settings.h"

typedef void (*ui_action_cb_t)(void);

// "Change settings" (from the held ⚙ button or an error screen) -> restart into setup.
void ui_set_change_settings_cb(ui_action_cb_t cb);

// Setup mode: join the panel's hotspot via QR code, then fill in the page on the phone.
void ui_show_setup(const char *ssid, const char *password);

// Status while starting up / for checkpoint C: network, IP, clock.
void ui_show_status(const char *title, const char *detail);
// weather_place: e.g. "Seattle, Washington, United States", or a problem description.
void ui_show_connected(const char *ssid, const char *ip, const char *timezone, bool time_synced,
                       const char *weather_place);

// Error with a "Change settings" button.
void ui_show_error(const char *title, const char *detail);

// "Change settings?" dialog (restarts into the setup hotspot).
void ui_confirm_change_settings(lv_event_t *e);

// The ⚙ menu (attach to the gear's LV_EVENT_CLICKED): manage from computer, change settings
// via the hotspot, sign out all computers.
void ui_open_settings_menu(lv_event_t *e);
// Run mode, online: the settings page is up at this IP (enables "Manage from computer").
void ui_set_web_address(const char *ip);
// A phone/computer signed in with the code (call with the LVGL lock held).
void ui_web_signed_in(void);

// The QR + code screens (on the top layer). MANAGE can be closed; FINISH (setup not done yet) and
// EXPIRED (Google sign-in withdrawn) stay until ui_close_qr().
typedef enum { UI_QR_MANAGE, UI_QR_FINISH, UI_QR_EXPIRED } ui_qr_kind_t;
void ui_show_qr(ui_qr_kind_t kind);
void ui_close_qr(void);

// The calendar (week view). Data comes from the model component; call ui_week_update() when it
// changes (with the LVGL lock held).
void ui_week_show(const hp_settings_t *settings);
void ui_week_update(void);
