// Add / edit event form (touch): on-screen keyboard, calendar pop-up, time wheels.
// Runs in the LVGL task; model callbacks (worker task) take the LVGL lock.
#include <stdio.h>
#include <string.h>

#include "board.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"
#include "model.h"
#include "ui.h"
#include "ui_internal.h"

#define C_BG 0x0f1115
#define C_CARD 0x181b22
#define C_LINE 0x2a2f3a
#define C_FG 0xe8eaed
#define C_MUTED 0x9aa0a6
#define C_ACCENT 0x8ab4f8
#define C_DANGER 0xf28b82

static const lv_font_t *F18 = &lv_font_montserrat_18, *F24 = &lv_font_montserrat_24;

static struct {
    lv_obj_t *root, *content, *kb, *error, *save;
    lv_obj_t *title, *location, *notes, *all_day, *who, *repeat, *until_on;
    lv_obj_t *date_btn, *start_btn, *end_btn, *until_btn, *time_row, *until_row, *repeat_row;
    hp_form_t form;           // date/times live here; text and choices are read from widgets on save
    bool editing, all_events, guest, custom_repeat, hide_repeat;
    char event_id[HP_ID_LEN];
    int span_days;
    int picking;              // 0 date, 1 until (calendar) / 0 start, 1 end (time)
    lv_obj_t *popup, *roller_h, *roller_m, *roller_ampm;
} F;

// --- small widgets ------------------------------------------------------------------------------

static lv_obj_t *text_label(lv_obj_t *parent, const char *s, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, s);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *row(lv_obj_t *parent, const char *caption) {
    lv_obj_t *r = lv_obj_create(parent);
    lv_obj_remove_style_all(r);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 12, 0);
    lv_obj_set_style_pad_ver(r, 4, 0);
    lv_obj_t *c = text_label(r, caption, F18, C_MUTED);
    lv_obj_set_width(c, 130);
    return r;
}

static lv_obj_t *pill_button(lv_obj_t *parent, const char *text, uint32_t bg, uint32_t fg, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_pad_ver(b, 12, 0);
    lv_obj_t *l = text_label(b, text, F24, fg);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

static void set_button_text(lv_obj_t *b, const char *text) { lv_label_set_text(lv_obj_get_child(b, 0), text); }

static lv_obj_t *textarea(lv_obj_t *parent, const char *value, bool one_line, int max_len);

static void fmt_date(hp_date_t d, char *out, size_t size) {
    struct tm tm = {.tm_year = d.y - 1900, .tm_mon = d.m - 1, .tm_mday = d.d, .tm_hour = 12};
    mktime(&tm);
    strftime(out, size, "%a %d %b %Y", &tm);
}

static void fmt_minutes(int m, char *out, size_t size) {
    if (m < 0) {
        snprintf(out, size, "--:--");
    } else if (ui_use_12h()) {
        int h = (m / 60) % 12 ? (m / 60) % 12 : 12;
        snprintf(out, size, "%d:%02d %s", h, m % 60, m < 720 ? "AM" : "PM");
    } else {
        snprintf(out, size, "%02d:%02d", m / 60, m % 60);
    }
}

static void refresh_labels(void) {
    char buf[48];
    fmt_date(F.form.date, buf, sizeof buf);
    set_button_text(F.date_btn, buf);
    fmt_minutes(F.form.start_min, buf, sizeof buf);
    set_button_text(F.start_btn, buf);
    fmt_minutes(F.form.end_min, buf, sizeof buf);
    set_button_text(F.end_btn, buf);
    if (F.form.has_until) fmt_date(F.form.until, buf, sizeof buf);
    else snprintf(buf, sizeof buf, "No end date");
    set_button_text(F.until_btn, buf);
    bool all_day = lv_obj_has_state(F.all_day, LV_STATE_CHECKED);
    if (all_day) lv_obj_add_flag(F.time_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(F.time_row, LV_OBJ_FLAG_HIDDEN);
    bool repeats = !F.hide_repeat && !F.custom_repeat && lv_dropdown_get_selected(F.repeat) != 0;
    if (repeats) lv_obj_remove_flag(F.until_row, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(F.until_row, LV_OBJ_FLAG_HIDDEN);
}

static void on_changed(lv_event_t *e) {
    (void)e;
    refresh_labels();
}

// --- keyboard -------------------------------------------------------------------------------------

static void on_textarea_focus(lv_event_t *e) {
    lv_obj_t *ta = lv_event_get_target_obj(e);
    lv_keyboard_set_textarea(F.kb, ta);
    lv_obj_remove_flag(F.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_height(F.content, BOARD_LCD_V_RES - 70 - 260);
    lv_obj_scroll_to_view(ta, LV_ANIM_ON);
}

static void hide_keyboard(void) {
    lv_obj_add_flag(F.kb, LV_OBJ_FLAG_HIDDEN);
    lv_keyboard_set_textarea(F.kb, NULL);
    lv_obj_set_height(F.content, BOARD_LCD_V_RES - 70);
}

static void on_keyboard(lv_event_t *e) {
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_READY || code == LV_EVENT_CANCEL) hide_keyboard();
}

static lv_obj_t *textarea(lv_obj_t *parent, const char *value, bool one_line, int max_len) {
    lv_obj_t *ta = lv_textarea_create(parent);
    lv_textarea_set_one_line(ta, one_line);
    lv_textarea_set_max_length(ta, max_len);
    lv_textarea_set_text(ta, value);
    lv_obj_set_flex_grow(ta, 1);
    if (!one_line) lv_obj_set_height(ta, 110);
    lv_obj_set_style_text_font(ta, F24, 0);
    lv_obj_set_style_bg_color(ta, lv_color_hex(C_BG), 0);
    lv_obj_set_style_text_color(ta, lv_color_hex(C_FG), 0);
    lv_obj_set_style_border_color(ta, lv_color_hex(C_LINE), 0);
    lv_obj_add_event_cb(ta, on_textarea_focus, LV_EVENT_FOCUSED, NULL);
    return ta;
}

// --- pop-ups: calendar and time wheels -------------------------------------------------------------

static void close_popup(void) {
    if (F.popup) lv_obj_delete(F.popup);
    F.popup = NULL;
}

static void on_popup_cancel(lv_event_t *e) {
    (void)e;
    close_popup();
}

static lv_obj_t *popup_card(int w, int h) {
    close_popup();
    hide_keyboard();
    F.popup = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(F.popup);
    lv_obj_set_size(F.popup, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(F.popup, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(F.popup, LV_OPA_60, 0);
    lv_obj_add_flag(F.popup, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_t *card = lv_obj_create(F.popup);
    lv_obj_set_size(card, w, h);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_color(card, lv_color_hex(C_ACCENT), 0);
    return card;
}

static void on_calendar_pick(lv_event_t *e) {
    lv_calendar_date_t d;
    if (lv_calendar_get_pressed_date(lv_event_get_target_obj(e), &d) != LV_RESULT_OK) return;
    hp_date_t picked = {d.year, d.month, d.day};
    if (F.picking == 0) F.form.date = picked;
    else F.form.until = picked, F.form.has_until = true;
    close_popup();
    refresh_labels();
}

static void on_no_end(lv_event_t *e) {
    (void)e;
    F.form.has_until = false;
    close_popup();
    refresh_labels();
}

static void open_calendar(int which) {
    F.picking = which;
    lv_obj_t *card = popup_card(520, 500);
    hp_date_t d = which == 0 || !F.form.has_until ? F.form.date : F.form.until;
    lv_obj_t *cal = lv_calendar_create(card);
    lv_obj_set_size(cal, 470, 380);
    lv_obj_align(cal, LV_ALIGN_TOP_MID, 0, 0);
    lv_calendar_set_today_date(cal, d.y, d.m, d.d);
    lv_calendar_set_showed_date(cal, d.y, d.m);
    lv_calendar_header_arrow_create(cal);
    lv_obj_set_style_text_font(cal, F18, 0);
    lv_obj_add_event_cb(cal, on_calendar_pick, LV_EVENT_VALUE_CHANGED, NULL);
    lv_obj_t *buttons = lv_obj_create(card);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(buttons, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 12, 0);
    if (which == 1) pill_button(buttons, "No end date", C_LINE, C_FG, on_no_end);
    pill_button(buttons, "Cancel", C_LINE, C_FG, on_popup_cancel);
}

static void on_date(lv_event_t *e) { (void)e; open_calendar(0); }
static void on_until(lv_event_t *e) { (void)e; open_calendar(1); }

static void on_time_ok(lv_event_t *e) {
    (void)e;
    int h = (int)lv_roller_get_selected(F.roller_h), m = (int)lv_roller_get_selected(F.roller_m) * 5;
    if (ui_use_12h()) h = (h % 12) + (lv_roller_get_selected(F.roller_ampm) ? 12 : 0);  // wheel shows 12,1..11
    int minutes = h * 60 + m;
    if (F.picking == 0) {
        int length = F.form.end_min > F.form.start_min ? F.form.end_min - F.form.start_min : 60;
        F.form.start_min = minutes;
        F.form.end_min = minutes + length < 24 * 60 ? minutes + length : 23 * 60 + 55;  // keep the length
    } else {
        F.form.end_min = minutes;
    }
    close_popup();
    refresh_labels();
}

static void open_time(int which) {
    F.picking = which;
    int current = which == 0 ? F.form.start_min : F.form.end_min;
    if (current < 0) current = 9 * 60;
    lv_obj_t *card = popup_card(460, 360);
    lv_obj_t *wheels = lv_obj_create(card);
    lv_obj_remove_style_all(wheels);
    lv_obj_set_size(wheels, lv_pct(100), 230);
    lv_obj_set_flex_flow(wheels, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(wheels, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(wheels, 16, 0);
    static char hours12[] = "12\n1\n2\n3\n4\n5\n6\n7\n8\n9\n10\n11";
    static char hours24[24 * 3];
    if (!hours24[0])
        for (int i = 0; i < 24; i++) snprintf(hours24 + i * 3, 4, "%02d%s", i, i < 23 ? "\n" : "");
    static char minutes[12 * 3];
    if (!minutes[0])
        for (int i = 0; i < 12; i++) snprintf(minutes + i * 3, 4, "%02d%s", i * 5, i < 11 ? "\n" : "");
    F.roller_h = lv_roller_create(wheels);
    lv_roller_set_options(F.roller_h, ui_use_12h() ? hours12 : hours24, LV_ROLLER_MODE_NORMAL);
    F.roller_m = lv_roller_create(wheels);
    lv_roller_set_options(F.roller_m, minutes, LV_ROLLER_MODE_NORMAL);
    int h = current / 60;
    lv_roller_set_selected(F.roller_h, ui_use_12h() ? h % 12 : h, LV_ANIM_OFF);
    lv_roller_set_selected(F.roller_m, (current % 60) / 5, LV_ANIM_OFF);
    F.roller_ampm = NULL;
    if (ui_use_12h()) {
        F.roller_ampm = lv_roller_create(wheels);
        lv_roller_set_options(F.roller_ampm, "AM\nPM", LV_ROLLER_MODE_NORMAL);
        lv_roller_set_selected(F.roller_ampm, h >= 12, LV_ANIM_OFF);
    }
    lv_obj_t *rollers[] = {F.roller_h, F.roller_m, F.roller_ampm};
    for (int i = 0; i < 3 && rollers[i]; i++) {
        lv_obj_set_style_text_font(rollers[i], F24, 0);
        lv_roller_set_visible_row_count(rollers[i], 4);
    }
    lv_obj_t *buttons = lv_obj_create(card);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_size(buttons, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_align(buttons, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(buttons, LV_FLEX_ALIGN_END, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(buttons, 12, 0);
    pill_button(buttons, "Cancel", C_LINE, C_FG, on_popup_cancel);
    pill_button(buttons, "OK", C_ACCENT, 0x0b1220, on_time_ok);
}

static void on_start(lv_event_t *e) { (void)e; open_time(0); }
static void on_end(lv_event_t *e) { (void)e; open_time(1); }

// --- save / close ------------------------------------------------------------------------------------

static void close_form(void) {
    close_popup();
    if (F.root) lv_obj_delete(F.root);
    memset(&F, 0, sizeof F);
}

static void on_cancel(lv_event_t *e) { (void)e; close_form(); }

static void read_widgets(hp_form_t *f) {
    snprintf(f->title, sizeof f->title, "%s", lv_textarea_get_text(F.title));
    snprintf(f->location, sizeof f->location, "%s", lv_textarea_get_text(F.location));
    snprintf(f->description, sizeof f->description, "%s", lv_textarea_get_text(F.notes));
    f->all_day = lv_obj_has_state(F.all_day, LV_STATE_CHECKED);
    int who = (int)lv_dropdown_get_selected(F.who);
    const hp_config_t *cfg = model_config();
    snprintf(f->member, sizeof f->member, "%s", who > 0 && who <= cfg->member_count ? cfg->members[who - 1].name : "");
    if (F.hide_repeat || F.custom_repeat) {
        f->repeat = HP_REPEAT_NONE;  // kept by the edit logic (occurrence / custom rule)
        f->has_until = false;
    } else {
        f->repeat = (hp_repeat_t)lv_dropdown_get_selected(F.repeat);
        if (f->repeat == HP_REPEAT_NONE) f->has_until = false;
    }
}

// Model callbacks run in a worker task: take the LVGL lock before touching the UI.
static void on_saved(hp_edit_result_t r, const char *msg, const hp_event_details_t *d) {
    (void)d;
    if (!lvgl_port_lock(0)) return;
    if (F.root) {
        if (r == HP_EDIT_OK) {
            ui_toast(F.editing ? "Saved" : "Added");
            close_form();
        } else {
            lv_label_set_text(F.error, msg);
            lv_obj_remove_state(F.save, LV_STATE_DISABLED);
            set_button_text(F.save, "Save");
        }
    }
    lvgl_port_unlock();
}

static void on_save(lv_event_t *e) {
    (void)e;
    hp_form_t f = F.form;
    read_widgets(&f);
    const char *problem = hp_form_validate(&f);
    if (problem) {
        lv_label_set_text(F.error, problem);
        return;
    }
    hide_keyboard();
    lv_label_set_text(F.error, "");
    lv_obj_add_state(F.save, LV_STATE_DISABLED);
    set_button_text(F.save, "Saving...");
    if (F.editing) model_edit_update(F.event_id, &f, F.all_events, on_saved);
    else model_edit_add(&f, on_saved);
}

// --- building the form --------------------------------------------------------------------------------

static void build(const char *heading, const char *note) {
    F.root = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(F.root);
    lv_obj_set_size(F.root, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(F.root, lv_color_hex(C_BG), 0);
    lv_obj_set_style_bg_opa(F.root, LV_OPA_COVER, 0);
    lv_obj_add_flag(F.root, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *head = lv_obj_create(F.root);
    lv_obj_remove_style_all(head);
    lv_obj_set_size(head, BOARD_LCD_H_RES, 70);
    lv_obj_t *h = text_label(head, heading, F24, C_FG);
    lv_obj_align(h, LV_ALIGN_LEFT_MID, 24, 0);
    lv_obj_t *buttons = lv_obj_create(head);
    lv_obj_remove_style_all(buttons);
    lv_obj_set_size(buttons, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_align(buttons, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_flex_flow(buttons, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(buttons, 10, 0);
    pill_button(buttons, "Cancel", C_LINE, C_FG, on_cancel);
    F.save = pill_button(buttons, "Save", C_ACCENT, 0x0b1220, on_save);

    F.content = lv_obj_create(F.root);
    lv_obj_remove_style_all(F.content);
    lv_obj_set_size(F.content, BOARD_LCD_H_RES, BOARD_LCD_V_RES - 70);
    lv_obj_set_pos(F.content, 0, 70);
    lv_obj_set_style_pad_hor(F.content, 24, 0);
    lv_obj_set_style_pad_bottom(F.content, 24, 0);
    lv_obj_set_flex_flow(F.content, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(F.content, 6, 0);
    lv_obj_set_scroll_dir(F.content, LV_DIR_VER);

    if (note) {
        lv_obj_t *n = text_label(F.content, note, F18, C_ACCENT);
        lv_obj_set_width(n, lv_pct(100));
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    }
    F.error = text_label(F.content, "", F18, C_DANGER);
    lv_obj_set_width(F.error, lv_pct(100));
    lv_label_set_long_mode(F.error, LV_LABEL_LONG_WRAP);

    lv_obj_t *r = row(F.content, "Title");
    F.title = textarea(r, F.form.title, true, HP_TITLE_LEN - 1);

    r = row(F.content, "Date");
    F.date_btn = pill_button(r, "", C_CARD, C_FG, on_date);
    text_label(r, "All day", F18, C_MUTED);
    F.all_day = lv_switch_create(r);
    if (F.form.all_day) lv_obj_add_state(F.all_day, LV_STATE_CHECKED);
    lv_obj_add_event_cb(F.all_day, on_changed, LV_EVENT_VALUE_CHANGED, NULL);
    if (F.span_days > 1) {
        char span[48];
        snprintf(span, sizeof span, "Lasts %d days", F.span_days);
        text_label(r, span, F18, C_MUTED);
    }

    F.time_row = row(F.content, "Time");
    F.start_btn = pill_button(F.time_row, "", C_CARD, C_FG, on_start);
    text_label(F.time_row, "to", F18, C_MUTED);
    F.end_btn = pill_button(F.time_row, "", C_CARD, C_FG, on_end);

    r = row(F.content, "Who");
    F.who = lv_dropdown_create(r);
    char options[HP_MAX_MEMBERS * (HP_NAME_LEN + 1) + 16] = "Whole family";
    const hp_config_t *cfg = model_config();
    int selected = 0;
    for (int i = 0; i < cfg->member_count; i++) {
        strcat(options, "\n");
        strcat(options, cfg->members[i].name);
        if (strcmp(cfg->members[i].name, F.form.member) == 0) selected = i + 1;
    }
    lv_dropdown_set_options(F.who, options);
    lv_dropdown_set_selected(F.who, selected);
    lv_obj_set_width(F.who, 300);
    lv_obj_set_style_text_font(F.who, F24, 0);

    F.repeat_row = row(F.content, "Repeat");
    F.repeat = lv_dropdown_create(F.repeat_row);
    lv_dropdown_set_options(F.repeat, "Does not repeat\nDaily\nWeekly\nMonthly\nYearly");
    lv_dropdown_set_selected(F.repeat, F.form.repeat);
    lv_obj_set_width(F.repeat, 300);
    lv_obj_set_style_text_font(F.repeat, F24, 0);
    lv_obj_add_event_cb(F.repeat, on_changed, LV_EVENT_VALUE_CHANGED, NULL);
    if (F.custom_repeat) {
        lv_obj_add_flag(F.repeat, LV_OBJ_FLAG_HIDDEN);
        text_label(F.repeat_row, "Custom repeat (kept as is; change it in Google Calendar)", F18, C_MUTED);
    }
    if (F.hide_repeat) lv_obj_add_flag(F.repeat_row, LV_OBJ_FLAG_HIDDEN);

    F.until_row = row(F.content, "Until");
    F.until_btn = pill_button(F.until_row, "", C_CARD, C_FG, on_until);

    r = row(F.content, "Location");
    F.location = textarea(r, F.form.location, true, sizeof F.form.location - 1);
    r = row(F.content, "Notes");
    F.notes = textarea(r, F.form.description, false, sizeof F.form.description - 1);

    F.kb = lv_keyboard_create(F.root);
    lv_obj_set_size(F.kb, BOARD_LCD_H_RES, 260);
    lv_obj_align(F.kb, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_add_flag(F.kb, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_event_cb(F.kb, on_keyboard, LV_EVENT_ALL, NULL);

    if (F.guest) {  // only "who" can change on events someone else organizes
        lv_obj_t *locked[] = {F.title, F.date_btn, F.all_day, F.start_btn, F.end_btn, F.repeat, F.until_btn, F.location, F.notes};
        for (size_t i = 0; i < sizeof locked / sizeof locked[0]; i++) {
            lv_obj_add_state(locked[i], LV_STATE_DISABLED);
            lv_obj_set_style_opa(locked[i], LV_OPA_50, 0);
        }
    }
    refresh_labels();
}

void ui_form_open_add(hp_date_t date) {
    close_form();
    memset(&F.form, 0, sizeof F.form);
    F.form.date = date;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int start = (tm.tm_hour + 1) * 60;  // next full hour
    if (start > 22 * 60) start = 9 * 60;
    F.form.start_min = start;
    F.form.end_min = start + 60;
    F.span_days = 1;
    build("Add family event", NULL);
}

void ui_form_open_edit(const char *event_id, const hp_event_details_t *d, bool all_events) {
    close_form();
    F.form = d->form;
    F.editing = true;
    F.all_events = all_events;
    F.guest = !d->owned;
    F.custom_repeat = d->custom_repeat;
    F.hide_repeat = d->recurring && !all_events;
    F.span_days = d->span_days;
    snprintf(F.event_id, sizeof F.event_id, "%s", event_id);
    if (F.form.start_min < 0) F.form.start_min = 9 * 60, F.form.end_min = 10 * 60;
    build(all_events ? "Edit all events in the series" : "Edit event",
          F.guest ? "Created by someone else. Only they can change the title, time or place, but you can change who it's for."
                  : NULL);
}
