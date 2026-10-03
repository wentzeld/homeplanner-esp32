// Setup, status and error screens.
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "lvgl.h"
#include "ui.h"

#define C_BG 0x0f1115
#define C_CARD 0x181b22
#define C_FG 0xe8eaed
#define C_MUTED 0x9aa0a6
#define C_ACCENT 0x8ab4f8
#define C_DANGER 0xf28b82

static ui_action_cb_t s_change_settings;
static lv_obj_t *s_clock;
static lv_timer_t *s_clock_timer;

void ui_set_change_settings_cb(ui_action_cb_t cb) { s_change_settings = cb; }

static lv_obj_t *new_screen(void) {
    if (s_clock_timer) {
        lv_timer_delete(s_clock_timer);
        s_clock_timer = NULL;
    }
    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    return scr;
}

static lv_obj_t *text(lv_obj_t *parent, const char *s, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *label, uint32_t bg, uint32_t fg) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_pad_hor(b, 24, 0);
    lv_obj_set_style_pad_ver(b, 14, 0);
    lv_obj_t *l = text(b, label, &lv_font_montserrat_24, fg);
    lv_obj_center(l);
    return b;
}

static void on_change_confirmed(lv_event_t *e) {
    (void)e;
    if (s_change_settings) s_change_settings();
}

static void close_parent(lv_event_t *e) { lv_obj_delete(lv_obj_get_parent(lv_event_get_target_obj(e))); }

void ui_confirm_change_settings(lv_event_t *e) {
    (void)e;
    lv_obj_t *box = lv_obj_create(lv_layer_top());
    lv_obj_set_size(box, 640, 260);
    lv_obj_center(box);
    lv_obj_set_style_bg_color(box, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_color(box, lv_color_hex(C_ACCENT), 0);
    lv_obj_t *t = text(box, "Change settings?", &lv_font_montserrat_32, C_FG);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 10, 10);
    lv_obj_t *d = text(box, "The panel restarts its setup hotspot so you can\nchange Wi-Fi, family members or the Google key.",
                       &lv_font_montserrat_18, C_MUTED);
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 10, 60);
    lv_obj_t *cancel = button(box, "Cancel", C_BG, C_FG);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 10, -10);
    lv_obj_add_event_cb(cancel, close_parent, LV_EVENT_CLICKED, NULL);
    lv_obj_t *go = button(box, "Change settings", C_ACCENT, 0x0b1220);
    lv_obj_align(go, LV_ALIGN_BOTTOM_RIGHT, -10, -10);
    lv_obj_add_event_cb(go, on_change_confirmed, LV_EVENT_CLICKED, NULL);
}

// ⚙ in the corner: opens the settings menu.
static void add_settings_button(lv_obj_t *scr) {
    lv_obj_t *b = button(scr, LV_SYMBOL_SETTINGS, C_CARD, C_FG);
    lv_obj_align(b, LV_ALIGN_TOP_RIGHT, -24, 24);
    lv_obj_add_event_cb(b, ui_open_settings_menu, LV_EVENT_CLICKED, NULL);
}

void ui_show_setup(const char *ssid, const char *password) {
    lv_obj_t *scr = new_screen();
    lv_obj_t *title = text(scr, "Set up HomePlanner", &lv_font_montserrat_48, C_FG);
    lv_obj_align(title, LV_ALIGN_TOP_LEFT, 48, 40);

    char qr_data[128];
    snprintf(qr_data, sizeof qr_data, "WIFI:T:WPA;S:%s;P:%s;;", ssid, password);
    lv_obj_t *qr = lv_qrcode_create(scr);
    lv_qrcode_set_size(qr, 300);
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    lv_qrcode_update(qr, qr_data, strlen(qr_data));
    lv_obj_set_style_border_color(qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(qr, 12, 0);
    lv_obj_align(qr, LV_ALIGN_TOP_RIGHT, -60, 150);

    char steps[512];
    snprintf(steps, sizeof steps,
             "1.  On your phone, scan this QR code to join\n"
             "     the panel's own Wi-Fi.\n\n"
             "     Or join it by hand:\n"
             "     Network:   %s\n"
             "     Password:  %s\n\n"
             "2.  The setup page opens by itself.\n"
             "     If not, open  http://192.168.4.1\n\n"
             "3.  Fill it in and tap  Save and start.",
             ssid, password);
    lv_obj_t *t = text(scr, steps, &lv_font_montserrat_24, C_FG);
    lv_obj_set_style_text_line_space(t, 6, 0);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 48, 150);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);  // frees the previous screen
}

void ui_show_status(const char *title, const char *detail) {
    lv_obj_t *scr = new_screen();
    lv_obj_t *t = text(scr, title, &lv_font_montserrat_48, C_FG);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 48, 40);
    lv_obj_t *d = text(scr, detail ? detail : "", &lv_font_montserrat_24, C_MUTED);
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 48, 120);
    add_settings_button(scr);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);  // frees the previous screen
}

static void tick_clock(lv_timer_t *t) {
    (void)t;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[64];
    strftime(buf, sizeof buf, "%A %d %B %Y   %H:%M:%S", &tm);
    lv_label_set_text(s_clock, buf);
}

void ui_show_connected(const char *ssid, const char *ip, const char *timezone, bool time_synced,
                       const char *weather_place) {
    lv_obj_t *scr = new_screen();
    lv_obj_t *t = text(scr, "HomePlanner is online", &lv_font_montserrat_48, C_FG);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 48, 40);
    char info[384];
    snprintf(info, sizeof info, "Wi-Fi:  %s\nAddress:  %s\nTimezone:  %s\nWeather for:  %s%s", ssid, ip, timezone,
             weather_place, time_synced ? "" : "\n\nClock not synced yet (no internet time server reached).");
    lv_obj_t *d = text(scr, info, &lv_font_montserrat_24, C_MUTED);
    lv_obj_set_style_text_line_space(d, 8, 0);
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 48, 130);
    s_clock = text(scr, "", &lv_font_montserrat_32, C_ACCENT);
    lv_obj_align(s_clock, LV_ALIGN_TOP_LEFT, 48, 340);
    lv_obj_t *n = text(scr, "The calendar is loading. Tap the gear for settings.",
                       &lv_font_montserrat_18, C_MUTED);
    lv_obj_align(n, LV_ALIGN_BOTTOM_LEFT, 48, -40);
    add_settings_button(scr);
    s_clock_timer = lv_timer_create(tick_clock, 1000, NULL);
    tick_clock(NULL);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);  // frees the previous screen
}

void ui_show_error(const char *title, const char *detail) {
    lv_obj_t *scr = new_screen();
    lv_obj_t *t = text(scr, title, &lv_font_montserrat_48, C_DANGER);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 48, 40);
    lv_obj_t *d = text(scr, detail, &lv_font_montserrat_24, C_FG);
    lv_obj_set_width(d, 900);
    lv_label_set_long_mode(d, LV_LABEL_LONG_WRAP);
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 48, 130);
    lv_obj_t *b = button(scr, "Change settings", C_ACCENT, 0x0b1220);
    lv_obj_align(b, LV_ALIGN_BOTTOM_LEFT, 48, -48);
    lv_obj_add_event_cb(b, on_change_confirmed, LV_EVENT_CLICKED, NULL);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);  // frees the previous screen
}
