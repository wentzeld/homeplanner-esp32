// The ⚙ menu (tap) and the "Manage from computer" screen with the one-time sign-in code.
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "lvgl.h"
#include "ui.h"
#include "ui_internal.h"
#include "update.h"
#include "web.h"

#define C_SHADE 0x000000
#define C_CARD 0x181b22
#define C_LINE 0x2a2f3a
#define C_FG 0xe8eaed
#define C_MUTED 0x9aa0a6
#define C_ACCENT 0x8ab4f8
#define C_DANGER 0xf28b82

static char s_ip[16];  // "" until the settings page is up (run mode, online)
static struct {
    lv_obj_t *shade, *code, *countdown, *renew, *qr, *status;
    lv_timer_t *timer;
    time_t expires;
    ui_qr_kind_t kind;
} C;

void ui_set_web_address(const char *ip) { snprintf(s_ip, sizeof s_ip, "%s", ip ? ip : ""); }

static lv_obj_t *label(lv_obj_t *parent, const char *s, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *button(lv_obj_t *parent, const char *text, uint32_t bg, uint32_t fg, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_pad_hor(b, 24, 0);
    lv_obj_set_style_pad_ver(b, 16, 0);
    lv_obj_t *l = label(b, text, &lv_font_montserrat_24, fg);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

// A dimmed full-screen layer with a card in the middle; tapping outside the card closes it.
static void close_shade_cb(lv_event_t *e) {
    if (lv_event_get_target(e) == lv_event_get_current_target(e)) lv_obj_delete(lv_event_get_current_target_obj(e));
}

static lv_obj_t *modal(int w, int h, lv_obj_t **card_out, bool tap_outside_closes) {
    lv_obj_t *shade = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(shade);
    lv_obj_set_size(shade, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(shade, lv_color_hex(C_SHADE), 0);
    lv_obj_set_style_bg_opa(shade, LV_OPA_60, 0);
    lv_obj_add_flag(shade, LV_OBJ_FLAG_CLICKABLE);
    if (tap_outside_closes) lv_obj_add_event_cb(shade, close_shade_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *card = lv_obj_create(shade);
    lv_obj_set_size(card, w, h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    *card_out = card;
    return shade;
}

static lv_obj_t *shade_of(lv_event_t *e) {  // the modal a button sits in
    lv_obj_t *o = lv_event_get_target_obj(e);
    while (lv_obj_get_parent(o) && lv_obj_get_parent(o) != lv_layer_top()) o = lv_obj_get_parent(o);
    return o;
}

static void close_modal(lv_event_t *e) { lv_obj_delete(shade_of(e)); }

// --- QR screens: manage from computer / finish setup / sign in again -------------------------------

static void close_code_screen(void) {
    if (C.timer) lv_timer_delete(C.timer);
    if (C.shade) lv_obj_delete(C.shade);
    memset(&C, 0, sizeof C);
}

static void show_new_code(void) {
    char code[7], spaced[8], url[64];
    web_new_code(code, &C.expires);
    snprintf(spaced, sizeof spaced, "%.3s %.3s", code, code + 3);
    lv_label_set_text(C.code, spaced);
    snprintf(url, sizeof url, "http://%s/?code=%s", s_ip, code);  // opens the page signed in
    lv_qrcode_update(C.qr, url, strlen(url));
    lv_obj_add_flag(C.renew, LV_OBJ_FLAG_HIDDEN);
    lv_timer_resume(C.timer);
}

static void tick_code(lv_timer_t *t) {
    (void)t;
    long left = (long)(C.expires - time(NULL));
    char buf[64];
    if (left > 0) {
        snprintf(buf, sizeof buf, "The code works for %ld:%02ld", left / 60, left % 60);
        lv_label_set_text(C.countdown, buf);
        return;
    }
    if (C.kind != UI_QR_MANAGE) {  // the setup screens stay up: just make a new code
        show_new_code();
        return;
    }
    lv_label_set_text(C.code, "------");
    lv_label_set_text(C.countdown, "The code expired.");
    lv_obj_remove_flag(C.renew, LV_OBJ_FLAG_HIDDEN);
    lv_timer_pause(C.timer);
}

static void on_renew(lv_event_t *e) {
    (void)e;
    show_new_code();
}

static void on_code_close(lv_event_t *e) {
    (void)e;
    web_cancel_code();
    close_code_screen();
}

void ui_show_qr(ui_qr_kind_t kind) {
    close_code_screen();
    C.kind = kind;
    lv_obj_t *card;
    C.shade = modal(960, 540, &card, false);
    if (kind != UI_QR_MANAGE) lv_obj_set_style_bg_opa(C.shade, LV_OPA_COVER, 0);  // a screen of its own
    static const char *TITLES[] = {"Manage from your phone or computer", "Finish setup on your phone", "Sign in to Google again"};
    static const char *INTROS[] = {
        "Scan this code with a phone on the same Wi-Fi as the panel.",
        "The panel is online. Scan this code with your phone (on the\nsame Wi-Fi) to sign in to Google and choose your calendar.",
        "The panel lost access to your Google calendar (the sign-in\nwas withdrawn or expired). Scan this code to sign in again.",
    };
    lv_obj_t *t = label(card, TITLES[kind], &lv_font_montserrat_32, C_FG);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    char steps[384];
    snprintf(steps, sizeof steps, "%s\n\nOr on a computer, open\n      http://homeplanner.local\n      (or http://%s)\nand enter this code:",
             INTROS[kind], s_ip);
    lv_obj_t *st = label(card, steps, &lv_font_montserrat_18, C_FG);
    lv_obj_set_style_text_line_space(st, 6, 0);
    lv_obj_align(st, LV_ALIGN_TOP_LEFT, 0, 60);
    C.code = label(card, "", &lv_font_montserrat_48, C_ACCENT);
    lv_obj_set_style_text_letter_space(C.code, 10, 0);
    lv_obj_align(C.code, LV_ALIGN_TOP_LEFT, 30, 300);
    C.countdown = label(card, "", &lv_font_montserrat_18, C_MUTED);
    lv_obj_align(C.countdown, LV_ALIGN_TOP_LEFT, 30, 370);
    C.status = label(card, "", &lv_font_montserrat_18, 0x81c995);
    lv_obj_align(C.status, LV_ALIGN_BOTTOM_LEFT, 0, -8);
    C.qr = lv_qrcode_create(card);
    lv_qrcode_set_size(C.qr, 300);
    lv_qrcode_set_dark_color(C.qr, lv_color_black());
    lv_qrcode_set_light_color(C.qr, lv_color_white());
    lv_obj_set_style_border_color(C.qr, lv_color_white(), 0);
    lv_obj_set_style_border_width(C.qr, 12, 0);
    lv_obj_align(C.qr, LV_ALIGN_TOP_RIGHT, 0, 40);
    C.renew = button(card, "New code", C_CARD, C_FG, on_renew);
    lv_obj_align(C.renew, LV_ALIGN_TOP_LEFT, 280, 300);
    if (kind == UI_QR_MANAGE) {
        lv_obj_t *close = button(card, "Close", C_ACCENT, 0x0b1220, on_code_close);
        lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
        lv_obj_t *note = label(card, "Signed-in devices stay signed in for 90 days.", &lv_font_montserrat_18, C_MUTED);
        lv_obj_align(note, LV_ALIGN_BOTTOM_LEFT, 0, -8);
    }
    C.timer = lv_timer_create(tick_code, 1000, NULL);
    show_new_code();
    tick_code(NULL);
}

void ui_close_qr(void) { close_code_screen(); }

void ui_web_signed_in(void) {
    if (!C.shade) return;
    if (C.kind == UI_QR_MANAGE) {
        close_code_screen();
        ui_toast("Signed in");
        return;
    }
    lv_label_set_text(C.status, LV_SYMBOL_OK "  Connected. Continue on your phone.");
}

static void open_code_screen(void) { ui_show_qr(UI_QR_MANAGE); }

// --- Sign out all computers ----------------------------------------------------------------------------

static void on_sign_out_confirmed(lv_event_t *e) {
    web_sign_out_all();
    close_modal(e);
    ui_toast("All computers are signed out");
}

static void confirm_sign_out(void) {
    lv_obj_t *card;
    modal(640, 280, &card, true);
    lv_obj_t *t = label(card, "Sign out all computers?", &lv_font_montserrat_32, C_FG);
    lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
    char d[160];
    int n = web_signed_in_count();
    snprintf(d, sizeof d, "%d %s signed in. To use the settings page again,\na computer needs a new code from the panel.", n,
             n == 1 ? "computer is" : "computers are");
    lv_obj_t *dl = label(card, d, &lv_font_montserrat_18, C_MUTED);
    lv_obj_align(dl, LV_ALIGN_TOP_LEFT, 0, 56);
    lv_obj_t *cancel = button(card, "Cancel", C_CARD, C_FG, close_modal);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *go = button(card, "Sign out all", C_DANGER, 0x0b1220, on_sign_out_confirmed);
    lv_obj_align(go, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

// --- Software update -------------------------------------------------------------------------------------

static struct {
    lv_obj_t *shade, *status, *install, *check;
} U;
static lv_obj_t *s_progress, *s_progress_bar, *s_progress_label;

static void close_update_card(void) {
    if (U.shade) lv_obj_delete(U.shade);
    memset(&U, 0, sizeof U);
}

static void on_update_close(lv_event_t *e) {
    (void)e;
    close_update_card();
}

static void refresh_update_card(void) {
    if (!U.shade) return;
    update_status_t st = update_status();
    char text[900];
    switch (st.state) {
        case UPDATE_AVAILABLE:
            snprintf(text, sizeof text, "Version %s is available.\n\n%.600s", st.latest, st.notes);
            break;
        case UPDATE_CHECKING: snprintf(text, sizeof text, "Checking for updates..."); break;
        case UPDATE_UP_TO_DATE:
        case UPDATE_FAILED: snprintf(text, sizeof text, "%s", st.message); break;
        default: snprintf(text, sizeof text, "Tap Check now to look for a new version."); break;
    }
    lv_label_set_text(U.status, text);
    if (st.state == UPDATE_AVAILABLE) lv_obj_remove_flag(U.install, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(U.install, LV_OBJ_FLAG_HIDDEN);
}

static void on_update_check(lv_event_t *e) {
    (void)e;
    update_check_now();
}

static void on_update_install_confirmed(lv_event_t *e) {
    close_modal(e);
    char err[160];
    if (update_install_available(err, sizeof err) != ESP_OK) ui_toast(err);
}

static void on_update_install(lv_event_t *e) {
    (void)e;
    update_status_t st = update_status();
    close_update_card();
    lv_obj_t *card;
    modal(640, 280, &card, true);
    char t[64];
    snprintf(t, sizeof t, "Install version %s?", st.latest);
    lv_obj_t *tl = label(card, t, &lv_font_montserrat_32, C_FG);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 0, 0);
    lv_obj_t *d = label(card, "The panel downloads it and restarts (about a minute).\nIf it doesn't work, it goes back to this version by itself.",
                        &lv_font_montserrat_18, C_MUTED);
    lv_obj_align(d, LV_ALIGN_TOP_LEFT, 0, 56);
    lv_obj_t *cancel = button(card, "Cancel", C_CARD, C_FG, close_modal);
    lv_obj_align(cancel, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_t *go = button(card, "Install", C_ACCENT, 0x0b1220, on_update_install_confirmed);
    lv_obj_align(go, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
}

static void open_update_card(void) {
    close_update_card();
    lv_obj_t *card;
    U.shade = modal(720, 460, &card, true);
    char t[64];
    snprintf(t, sizeof t, "Software update  -  version %s", update_current_version());
    lv_obj_t *tl = label(card, t, &lv_font_montserrat_32, C_FG);
    lv_obj_align(tl, LV_ALIGN_TOP_LEFT, 0, 0);
    U.status = label(card, "", &lv_font_montserrat_18, C_FG);
    lv_obj_set_width(U.status, 660);
    lv_label_set_long_mode(U.status, LV_LABEL_LONG_WRAP);
    lv_obj_align(U.status, LV_ALIGN_TOP_LEFT, 0, 60);
    U.check = button(card, "Check now", C_CARD, C_FG, on_update_check);
    lv_obj_align(U.check, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    U.install = button(card, "Install", C_ACCENT, 0x0b1220, on_update_install);
    lv_obj_align(U.install, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_t *close = button(card, "Close", C_CARD, C_FG, on_update_close);
    lv_obj_align(close, LV_ALIGN_BOTTOM_RIGHT, 0, 0);
    refresh_update_card();
}

// Called (LVGL lock held) whenever the update status changes: the progress screen for any install.
void ui_update_changed(void) {
    update_status_t st = update_status();
    if (st.state == UPDATE_INSTALLING) {
        if (!s_progress) {
            close_update_card();
            lv_obj_t *card;
            s_progress = modal(640, 240, &card, false);
            lv_obj_set_style_bg_opa(s_progress, LV_OPA_COVER, 0);
            lv_obj_t *t = label(card, "Updating HomePlanner", &lv_font_montserrat_32, C_FG);
            lv_obj_align(t, LV_ALIGN_TOP_LEFT, 0, 0);
            s_progress_label = label(card, "", &lv_font_montserrat_18, C_MUTED);
            lv_obj_align(s_progress_label, LV_ALIGN_TOP_LEFT, 0, 60);
            s_progress_bar = lv_bar_create(card);
            lv_obj_set_size(s_progress_bar, 580, 24);
            lv_obj_align(s_progress_bar, LV_ALIGN_BOTTOM_MID, 0, -20);
            lv_obj_set_style_bg_color(s_progress_bar, lv_color_hex(C_ACCENT), LV_PART_INDICATOR);
        }
        char text[200];
        snprintf(text, sizeof text, "%s  %d%%\nPlease don't unplug the panel.", st.message, st.percent);
        lv_label_set_text(s_progress_label, text);
        lv_bar_set_value(s_progress_bar, st.percent, LV_ANIM_OFF);
        return;
    }
    if (s_progress) {  // failed or done without a restart
        lv_obj_delete(s_progress);
        s_progress = NULL;
        if (st.state == UPDATE_FAILED) ui_toast(st.message);
    }
    refresh_update_card();
}

static void on_software_update(lv_event_t *e) {
    close_modal(e);
    open_update_card();
}

// --- The menu ----------------------------------------------------------------------------------------

static void on_manage(lv_event_t *e) {
    close_modal(e);
    if (!s_ip[0]) {
        ui_toast("The panel isn't online yet");
        return;
    }
    open_code_screen();
}

static void on_change_settings(lv_event_t *e) {
    close_modal(e);
    ui_confirm_change_settings(NULL);
}

static void on_sign_out(lv_event_t *e) {
    close_modal(e);
    if (!s_ip[0]) {
        ui_toast("The panel isn't online yet");
        return;
    }
    confirm_sign_out();
}

void ui_open_settings_menu(lv_event_t *e) {
    (void)e;
    lv_obj_t *card;
    modal(560, LV_SIZE_CONTENT, &card, true);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 12, 0);
    char title[48];
    snprintf(title, sizeof title, "Settings  (version %s)", update_current_version());
    label(card, title, &lv_font_montserrat_32, C_FG);
    const struct {
        const char *text;
        lv_event_cb_t cb;
    } items[] = {
        {LV_SYMBOL_EDIT "  Manage from phone or computer", on_manage},
        {LV_SYMBOL_WIFI "  Change settings (hotspot)", on_change_settings},
        {LV_SYMBOL_CLOSE "  Sign out all computers", on_sign_out},
        {LV_SYMBOL_DOWNLOAD "  Software update", on_software_update},
        {"Close", close_modal},
    };
    const size_t last = sizeof items / sizeof items[0] - 1;
    for (size_t i = 0; i < sizeof items / sizeof items[0]; i++) {
        lv_obj_t *b = button(card, items[i].text, i == last ? C_ACCENT : C_CARD, i == last ? 0x0b1220 : C_FG, items[i].cb);
        lv_obj_set_width(b, LV_PCT(100));
        lv_obj_set_style_text_align(lv_obj_get_child(b, 0), LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(lv_obj_get_child(b, 0), LV_ALIGN_LEFT_MID, 0, 0);
    }
}
