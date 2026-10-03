// The calendar: Sunday-Saturday week view, legend filter, event details, screen sleep.
// All functions run in the LVGL task (or with the LVGL lock held).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "board.h"
#include "esp_heap_caps.h"
#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "model.h"
#include "ui.h"
#include "ui_internal.h"

#define C_BG 0x0f1115
#define C_CARD 0x181b22
#define C_TODAY 0x1d2331
#define C_LINE 0x2a2f3a
#define C_FG 0xe8eaed
#define C_MUTED 0x9aa0a6
#define C_ACCENT 0x8ab4f8
#define C_DANGER 0xf28b82
#define C_WEEKEND 0xc9b8ff

#define IDLE_BACK_TO_THIS_WEEK_MS (5 * 60 * 1000)
#define FILTER_RESET_MS (60 * 1000)
#define MAX_SHOWN 400
#define COL_W 137
#define COL_GAP 6
#define DAYS_Y 124
#define DAYS_H 420
#define SYNC_W 220     // sync status label, bottom right
#define SYNC_RIGHT 14
#define SYNC_TAP 10    // extra touch margin around it

static const hp_settings_t *s_cfg;
static hp_date_t s_week;  // Sunday of the shown week
static bool s_follow_today = true;
static char s_filter[HP_NAME_LEN + 1];  // "\x01" = whole-family events, "\x02<name>" = another calendar
static bool s_filter_on;
static bool s_use_12h;
static bool s_screen_off;
static hp_date_t s_rendered_today;

static lv_obj_t *s_clock, *s_date, *s_now_weather, *s_range, *s_days, *s_legend, *s_synced, *s_wake_catcher;
static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;
static struct {  // the event details card that is open
    lv_obj_t *shade, *status, *delete_btn, *scope_row;
    hp_event_t ev;
    int action;  // 1 edit, 2 delete
    bool armed_all, delete_armed;
} D;
static hp_event_t *s_shown;  // copies of the events on screen (PSRAM), so taps survive a background sync
static int s_shown_count;

static const lv_font_t *F14 = &lv_font_montserrat_14, *F18 = &lv_font_montserrat_18, *F24 = &lv_font_montserrat_24,
                       *F48 = &lv_font_montserrat_48;

// --- helpers -----------------------------------------------------------------------------------

static lv_color_t hex_color(const char *hex) { return lv_color_hex((uint32_t)strtoul(hex + 1, NULL, 16)); }

static lv_obj_t *label(lv_obj_t *parent, const char *text, const lv_font_t *font, uint32_t color) {
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    return l;
}

static lv_obj_t *box(lv_obj_t *parent) {
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static void fmt_time(int64_t t, char *out, size_t size) {
    time_t tt = (time_t)t;
    struct tm tm;
    localtime_r(&tt, &tm);
    if (!s_use_12h) {
        snprintf(out, size, "%02d:%02d", tm.tm_hour, tm.tm_min);
        return;
    }
    int h = tm.tm_hour % 12 ? tm.tm_hour % 12 : 12;
    const char *ampm = tm.tm_hour < 12 ? "AM" : "PM";
    if (tm.tm_min) snprintf(out, size, "%d:%02d %s", h, tm.tm_min, ampm);
    else snprintf(out, size, "%d %s", h, ampm);
}

static void fmt_day(hp_date_t d, const char *fmt, char *out, size_t size) {
    struct tm tm = {.tm_year = d.y - 1900, .tm_mon = d.m - 1, .tm_mday = d.d, .tm_hour = 12};
    mktime(&tm);
    strftime(out, size, fmt, &tm);
}

static hp_date_t today(void) { return hp_local_date(time(NULL)); }

static bool matches_filter(const hp_event_t *e) {
    if (!s_filter_on) return true;
    if (s_filter[0] == '\x01') return e->member[0] == '\0' && e->calendar[0] == '\0';
    if (s_filter[0] == '\x02') return strcmp(e->calendar, s_filter + 1) == 0;
    return strcmp(e->member, s_filter) == 0;
}

static int cmp_events(const void *a, const void *b) {
    const hp_event_t *x = *(const hp_event_t *const *)a, *y = *(const hp_event_t *const *)b;
    if (x->all_day != y->all_day) return x->all_day ? -1 : 1;
    if (x->start != y->start) return x->start < y->start ? -1 : 1;
    return strcmp(x->title, y->title);
}

// --- event details --------------------------------------------------------------------------------

bool ui_use_12h(void) { return s_use_12h; }

static void hide_toast(lv_timer_t *t) {
    (void)t;
    if (s_toast) lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_timer = NULL;
}

void ui_toast(const char *text) {
    if (!s_toast) {
        s_toast = lv_label_create(lv_layer_top());
        lv_obj_set_style_bg_color(s_toast, lv_color_hex(0x81c995), 0);
        lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(s_toast, lv_color_hex(0x0b1220), 0);
        lv_obj_set_style_text_font(s_toast, F24, 0);
        lv_obj_set_style_pad_hor(s_toast, 24, 0);
        lv_obj_set_style_pad_ver(s_toast, 12, 0);
        lv_obj_set_style_radius(s_toast, 10, 0);
    }
    lv_label_set_text(s_toast, text);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    lv_obj_align(s_toast, LV_ALIGN_BOTTOM_MID, 0, -70);
    lv_obj_move_foreground(s_toast);
    if (s_toast_timer) lv_timer_delete(s_toast_timer);
    s_toast_timer = lv_timer_create(hide_toast, 3000, NULL);
    lv_timer_set_repeat_count(s_toast_timer, 1);
}

void ui_notice(const char *text) {  // like a toast, but stays long enough to read a sentence
    ui_toast(text);
    lv_timer_set_period(s_toast_timer, 8000);
}

static void close_details(void) {
    if (D.shade) lv_obj_delete(D.shade);
    memset(&D, 0, sizeof D);
}

static void on_close_details(lv_event_t *e) {
    (void)e;
    close_details();
}

static void details_message(const char *text, bool error) {
    lv_label_set_text(D.status, text);
    lv_obj_set_style_text_color(D.status, lv_color_hex(error ? C_DANGER : C_MUTED), 0);
}

// Worker-task callbacks: take the LVGL lock before touching the UI.
static void on_details_loaded(hp_edit_result_t r, const char *msg, const hp_event_details_t *d) {
    if (!lvgl_port_lock(0)) return;
    if (D.shade) {
        if (r != HP_EDIT_OK) {
            details_message(msg, true);
        } else if (!d->form_editable) {
            details_message("Events that run past midnight can only be edited in Google Calendar.", true);
        } else {
            char id[HP_ID_LEN];
            snprintf(id, sizeof id, "%s", D.ev.id);
            bool all = D.armed_all;
            close_details();
            ui_form_open_edit(id, d, all);
        }
    }
    lvgl_port_unlock();
}

static void on_deleted(hp_edit_result_t r, const char *msg, const hp_event_details_t *d) {
    (void)d;
    if (!lvgl_port_lock(0)) return;
    if (D.shade) {
        if (r == HP_EDIT_OK) {
            close_details();
            ui_toast("Deleted");
        } else {
            details_message(msg, true);
        }
    }
    lvgl_port_unlock();
}

static void run_action(void) {
    lv_obj_add_flag(D.scope_row, LV_OBJ_FLAG_HIDDEN);
    if (D.action == 1) {
        details_message("Loading...", false);
        model_edit_details(D.ev.id, on_details_loaded);
    } else if (D.action == 2) {
        D.delete_armed = true;
        lv_label_set_text(lv_obj_get_child(D.delete_btn, 0), D.armed_all ? "Really delete all events?" : "Really delete?");
    }
}

static void on_scope(lv_event_t *e) {
    D.armed_all = (intptr_t)lv_event_get_user_data(e) == 1;
    run_action();
}

// Repeating events: ask "This event" / "All events" first.
static void ask_scope_then(int action) {
    D.action = action;
    if (!D.ev.recurring) {
        D.armed_all = false;
        run_action();
        return;
    }
    lv_obj_remove_flag(D.scope_row, LV_OBJ_FLAG_HIDDEN);
    lv_label_set_text(lv_obj_get_child(D.scope_row, 0), action == 1 ? "Edit which events?" : "Delete which events?");
}

static void on_edit(lv_event_t *e) {
    (void)e;
    ask_scope_then(1);
}

static void on_delete(lv_event_t *e) {
    (void)e;
    if (D.delete_armed) {  // second tap
        details_message("Deleting...", false);
        model_edit_delete(D.ev.id, D.armed_all, on_deleted);
        return;
    }
    ask_scope_then(2);
}

static lv_obj_t *card_button(lv_obj_t *parent, const char *text, uint32_t bg, uint32_t fg, lv_event_cb_t cb, void *data) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(bg), 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_style_pad_hor(b, 18, 0);
    lv_obj_set_style_pad_ver(b, 10, 0);
    lv_obj_t *l = label(b, text, F18, fg);
    lv_obj_center(l);
    lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, data);
    return b;
}

static lv_obj_t *button_row(lv_obj_t *parent) {
    lv_obj_t *r = box(parent);
    lv_obj_set_size(r, lv_pct(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(r, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(r, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(r, 10, 0);
    return r;
}

static void show_details(lv_event_t *e) {
    close_details();
    D.ev = s_shown[(intptr_t)lv_event_get_user_data(e)];
    const hp_event_t *ev = &D.ev;
    D.shade = box(lv_layer_top());
    lv_obj_set_size(D.shade, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
    lv_obj_set_style_bg_color(D.shade, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(D.shade, LV_OPA_60, 0);
    lv_obj_add_flag(D.shade, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(D.shade, on_close_details, LV_EVENT_CLICKED, NULL);

    lv_obj_t *card = lv_obj_create(D.shade);
    lv_obj_set_size(card, 660, LV_SIZE_CONTENT);
    lv_obj_center(card);
    lv_obj_set_style_bg_color(card, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_color(card, hex_color(ev->color), 0);
    lv_obj_set_style_border_width(card, 3, 0);
    lv_obj_set_style_radius(card, 14, 0);
    lv_obj_set_style_pad_all(card, 24, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(card, 10, 0);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);  // taps inside the card don't close it

    lv_obj_t *t = label(card, ev->title, F24, C_FG);
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);

    char when[160], a[48], b[24], day[48];
    fmt_day(hp_local_date(ev->start), "%A %d %B", day, sizeof day);
    if (ev->all_day) {
        hp_date_t last = hp_local_date(ev->end - 1);
        if (hp_date_cmp(last, hp_local_date(ev->start)) == 0) {
            snprintf(when, sizeof when, "%s, all day", day);
        } else {
            fmt_day(last, "%A %d %B", a, sizeof a);
            snprintf(when, sizeof when, "%s - %s", day, a);
        }
    } else {
        fmt_time(ev->start, a, sizeof a);
        fmt_time(ev->end, b, sizeof b);
        snprintf(when, sizeof when, "%s, %s%s%s", day, a, ev->end > ev->start ? " - " : "", ev->end > ev->start ? b : "");
    }
    label(card, when, F18, C_FG);
    if (ev->recurring) label(card, LV_SYMBOL_REFRESH "  Repeating event", F18, C_MUTED);
    if (ev->calendar[0]) {
        char from[HP_NAME_LEN + 32];
        snprintf(from, sizeof from, "From %s (read-only)", ev->calendar);
        label(card, from, F18, C_MUTED);
    } else {
        label(card, ev->member[0] ? ev->member : "Whole family", F18, C_MUTED);
    }
    if (ev->location[0]) {
        lv_obj_t *l = label(card, ev->location, F18, C_MUTED);
        lv_obj_set_width(l, lv_pct(100));
        lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    }
    if (ev->description[0]) {
        lv_obj_t *n = label(card, ev->description, F14, C_FG);
        lv_obj_set_width(n, lv_pct(100));
        lv_label_set_long_mode(n, LV_LABEL_LONG_WRAP);
    }
    if (!ev->owned && !ev->calendar[0]) label(card, "Created by someone else: you can change who it's for.", F14, C_ACCENT);

    D.scope_row = button_row(card);
    label(D.scope_row, "", F18, C_FG);
    card_button(D.scope_row, "This event", C_LINE, C_FG, on_scope, (void *)0);
    card_button(D.scope_row, "All events", C_LINE, C_FG, on_scope, (void *)1);
    lv_obj_add_flag(D.scope_row, LV_OBJ_FLAG_HIDDEN);

    D.status = label(card, "", F18, C_MUTED);
    lv_obj_set_width(D.status, lv_pct(100));
    lv_label_set_long_mode(D.status, LV_LABEL_LONG_WRAP);

    lv_obj_t *actions = button_row(card);
    if (ev->editable) {
        D.delete_btn = card_button(actions, "Delete", C_LINE, C_DANGER, on_delete, NULL);
        if (!ev->owned) lv_obj_add_flag(D.delete_btn, LV_OBJ_FLAG_HIDDEN);
        card_button(actions, "Edit", C_ACCENT, 0x0b1220, on_edit, NULL);
    }
    card_button(actions, "Close", C_LINE, C_FG, on_close_details, NULL);
}

// --- rendering ------------------------------------------------------------------------------------

static void add_event_card(lv_obj_t *col, int idx) {
    const hp_event_t *ev = &s_shown[idx];
    lv_color_t color = hex_color(ev->color);
    lv_obj_t *card = box(col);
    lv_obj_set_width(card, lv_pct(100));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(card, show_details, LV_EVENT_CLICKED, (void *)(intptr_t)idx);
    lv_obj_set_style_radius(card, 5, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    if (!matches_filter(ev)) lv_obj_set_style_opa(card, LV_OPA_20, 0);

    if (ev->all_day) {
        lv_obj_set_style_bg_color(card, color, 0);
        lv_obj_set_style_pad_hor(card, 6, 0);
        lv_obj_set_style_pad_ver(card, 3, 0);
        lv_obj_t *t = label(card, ev->title, F14, 0xffffff);
        lv_obj_set_width(t, lv_pct(100));
        lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
        return;
    }
    lv_obj_set_style_bg_color(card, lv_color_mix(color, lv_color_hex(C_CARD), LV_OPA_20), 0);
    lv_obj_set_style_border_side(card, LV_BORDER_SIDE_LEFT, 0);
    lv_obj_set_style_border_width(card, 4, 0);
    lv_obj_set_style_border_color(card, color, 0);
    lv_obj_set_style_pad_all(card, 5, 0);
    lv_obj_set_style_pad_left(card, 8, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    if (ev->end < time(NULL)) lv_obj_set_style_opa(card, matches_filter(ev) ? LV_OPA_50 : LV_OPA_20, 0);

    char a[24], b[24], times[56];
    fmt_time(ev->start, a, sizeof a);
    fmt_time(ev->end, b, sizeof b);
    if (ev->end > ev->start) snprintf(times, sizeof times, "%s - %s", a, b);
    else snprintf(times, sizeof times, "%s", a);
    label(card, times, F14, C_MUTED);
    lv_obj_t *t = label(card, ev->title, F18, C_FG);
    lv_obj_set_width(t, lv_pct(100));
    lv_label_set_long_mode(t, LV_LABEL_LONG_WRAP);
    if (ev->member[0] || ev->calendar[0]) label(card, ev->member[0] ? ev->member : ev->calendar, F14, C_MUTED);
}

static const weather_day_t *weather_for(const weather_report_t *w, hp_date_t d) {
    for (int i = 0; w->valid && i < w->day_count; i++)
        if (w->days[i].y == d.y && w->days[i].m == d.m && w->days[i].d == d.d) return &w->days[i];
    return NULL;
}

static void render_days(void) {
    lv_obj_clean(s_days);
    hp_date_t now_day = today();
    s_rendered_today = now_day;
    if (!s_shown) s_shown = heap_caps_calloc(MAX_SHOWN, sizeof(hp_event_t), MALLOC_CAP_SPIRAM);
    s_shown_count = 0;

    model_lock();
    int count = 0;
    const hp_event_t *events = model_events(&count);
    const weather_report_t *w = model_weather();
    for (int i = 0; i < 7; i++) {
        hp_date_t d = hp_date_add(s_week, i);
        int cmp = hp_date_cmp(d, now_day);
        lv_obj_t *col = lv_obj_create(s_days);
        lv_obj_set_size(col, COL_W, DAYS_H);
        lv_obj_set_pos(col, i * (COL_W + COL_GAP), 0);
        lv_obj_set_style_bg_color(col, lv_color_hex(cmp == 0 ? C_TODAY : C_CARD), 0);
        lv_obj_set_style_border_color(col, lv_color_hex(cmp == 0 ? C_ACCENT : C_LINE), 0);
        lv_obj_set_style_border_width(col, cmp == 0 ? 2 : 1, 0);
        lv_obj_set_style_radius(col, 10, 0);
        lv_obj_set_style_pad_all(col, 6, 0);
        lv_obj_set_style_pad_row(col, 5, 0);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_scroll_dir(col, LV_DIR_VER);
        lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);
        lv_obj_add_flag(col, LV_OBJ_FLAG_GESTURE_BUBBLE);
        if (cmp < 0) lv_obj_set_style_opa(col, LV_OPA_60, 0);

        char name[24], date[24];
        int wd = hp_weekday(d);
        if (cmp == 0) snprintf(name, sizeof name, "Today");
        else if (hp_date_diff(d, now_day) == 1) snprintf(name, sizeof name, "Tomorrow");
        else fmt_day(d, "%A", name, sizeof name);
        fmt_day(d, "%a %d %b", date, sizeof date);
        label(col, name, F18, cmp == 0 ? C_ACCENT : (wd == 0 || wd == 6) ? C_WEEKEND : C_FG);
        label(col, date, F14, C_MUTED);
        const weather_day_t *wday = weather_for(w, d);
        if (wday) {
            char wt[48];
            int n = snprintf(wt, sizeof wt, "%.0f\xc2\xb0/%.0f\xc2\xb0", wday->high, wday->low);
            if (wday->precip_chance >= 30) snprintf(wt + n, sizeof wt - n, "  %d%% rain", wday->precip_chance);
            label(col, wt, F14, C_MUTED);
            const char *desc = weather_describe(wday->code);
            if (desc[0]) label(col, desc, F14, C_MUTED);
        }

        // Events overlapping this day, all-day first.
        const hp_event_t *day_events[96];
        int n = 0;
        for (int k = 0; k < count && n < 96; k++)
            if (hp_overlaps_day(&events[k], d)) day_events[n++] = &events[k];
        qsort(day_events, n, sizeof day_events[0], cmp_events);
        if (n == 0) label(col, "Nothing planned", F14, C_MUTED);
        for (int k = 0; k < n && s_shown_count < MAX_SHOWN; k++) {
            s_shown[s_shown_count] = *day_events[k];
            add_event_card(col, s_shown_count++);
        }
    }
    model_unlock();

    char a[32], b[32], range[192];
    fmt_day(s_week, "%a %d %b", a, sizeof a);
    fmt_day(hp_date_add(s_week, 6), "%a %d %b", b, sizeof b);
    hp_date_t this_week = hp_week_start(now_day);
    int weeks = hp_date_diff(s_week, this_week) / 7;
    const char *prefix = weeks == 0 ? "This week  |  " : weeks == 1 ? "Next week  |  " : weeks == -1 ? "Last week  |  " : "";
    snprintf(range, sizeof range, "%s%s - %s%s%s", prefix, a, b, s_filter_on ? "    Showing " : "",
             s_filter_on ? (s_filter[0] == '\x01' ? "Family" : s_filter[0] == '\x02' ? s_filter + 1 : s_filter) : "");
    lv_label_set_text(s_range, range);
}

static void render_status(void) {
    model_lock();
    model_status_t st = model_status();
    const weather_report_t *w = model_weather();
    if (w->valid) {
        char now[64];
        snprintf(now, sizeof now, "%.0f\xc2\xb0%c  %s", w->temperature, w->unit, weather_describe(w->code));
        lv_label_set_text(s_now_weather, now);
    }
    model_unlock();
    char text[256], t[24];
    if (st.syncing && !st.last_synced) snprintf(text, sizeof text, "Syncing...");
    else if (st.error && st.network_error && st.last_synced) {
        fmt_time(st.last_synced, t, sizeof t);
        snprintf(text, sizeof text, "Can't reach Google - showing events from %s", t);
    } else if (st.error && st.network_error) snprintf(text, sizeof text, "Waiting for network...");
    else if (st.error) snprintf(text, sizeof text, "Calendar not connected: %s", st.message);
    else if (st.last_synced) {
        fmt_time(st.last_synced, t, sizeof t);
        snprintf(text, sizeof text, "Synced %s  " LV_SYMBOL_REFRESH, t);
    } else snprintf(text, sizeof text, "Syncing...");
    lv_label_set_text(s_synced, text);
    lv_obj_set_style_text_color(s_synced, lv_color_hex(st.error && !st.network_error ? C_DANGER : C_MUTED), 0);
}

static void render_legend(void);

static void set_week(hp_date_t sunday) {
    s_week = sunday;
    s_follow_today = hp_date_cmp(sunday, hp_week_start(today())) == 0;
    model_want_week(sunday);
    render_days();
}

// --- input ---------------------------------------------------------------------------------------

static void on_prev(lv_event_t *e) { (void)e; set_week(hp_date_add(s_week, -7)); }
static void on_next(lv_event_t *e) { (void)e; set_week(hp_date_add(s_week, 7)); }
static void on_today(lv_event_t *e) { (void)e; set_week(hp_week_start(today())); }
static void on_refresh(lv_event_t *e) {
    (void)e;
    lv_label_set_text(s_synced, "Syncing...");
    model_refresh_now();
}

static void on_add(lv_event_t *e) {
    (void)e;
    hp_date_t now_day = today();
    bool in_view = hp_date_cmp(now_day, s_week) >= 0 && hp_date_diff(now_day, s_week) < 7;
    ui_form_open_add(in_view ? now_day : s_week);
}

static void on_gesture(lv_event_t *e) {
    (void)e;
    lv_dir_t dir = lv_indev_get_gesture_dir(lv_indev_active());
    if (dir == LV_DIR_LEFT) set_week(hp_date_add(s_week, 7));
    else if (dir == LV_DIR_RIGHT) set_week(hp_date_add(s_week, -7));
}

static void on_legend(lv_event_t *e) {
    const char *name = lv_event_get_user_data(e);
    if (s_filter_on && strcmp(s_filter, name) == 0) s_filter_on = false;
    else {
        snprintf(s_filter, sizeof s_filter, "%s", name);
        s_filter_on = true;
    }
    render_legend();
    render_days();
}

static void legend_chip(const char *name, const char *key, uint32_t color) {
    lv_obj_t *chip = box(s_legend);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, 34);
    lv_obj_set_style_pad_hor(chip, 10, 0);
    lv_obj_set_style_radius(chip, 17, 0);
    lv_obj_add_flag(chip, LV_OBJ_FLAG_CLICKABLE);
    bool selected = s_filter_on && strcmp(s_filter, key) == 0;
    lv_obj_set_style_border_width(chip, selected ? 2 : 0, 0);
    lv_obj_set_style_border_color(chip, lv_color_hex(color), 0);
    if (s_filter_on && !selected) lv_obj_set_style_opa(chip, LV_OPA_40, 0);
    lv_obj_set_flex_flow(chip, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(chip, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(chip, 6, 0);
    lv_obj_t *dot = box(chip);
    lv_obj_set_size(dot, 14, 14);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(dot, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(dot, lv_color_hex(color), 0);
    label(chip, name, F18, selected ? C_FG : C_MUTED);
    lv_obj_add_event_cb(chip, on_legend, LV_EVENT_CLICKED, (void *)key);
}

static hp_calendar_t *s_cals;  // other calendars in the legend (PSRAM)
static char s_cal_keys[HP_MAX_CALENDARS][HP_NAME_LEN + 1];
static unsigned s_cals_version = ~0u;

static void render_legend(void) {
    lv_obj_clean(s_legend);
    for (int i = 0; i < s_cfg->cal.member_count; i++)
        legend_chip(s_cfg->cal.members[i].name, s_cfg->cal.members[i].name,
                    (uint32_t)strtoul(hp_color_hex(s_cfg->cal.members[i].color_id) + 1, NULL, 16));
    legend_chip("Family", "\x01", (uint32_t)strtoul(HP_FAMILY_COLOR + 1, NULL, 16));
    if (!s_cals) s_cals = heap_caps_calloc(HP_MAX_CALENDARS, sizeof *s_cals, MALLOC_CAP_SPIRAM);
    if (!s_cals) return;
    s_cals_version = model_calendars_version();
    int n = model_calendars(s_cals, HP_MAX_CALENDARS);
    for (int i = 0; i < n; i++) {
        snprintf(s_cal_keys[i], sizeof s_cal_keys[i], "\x02%s", s_cals[i].name);
        const char *hex = hp_palette_hex(s_cals[i].color);
        legend_chip(s_cals[i].name, s_cal_keys[i], (uint32_t)strtoul((hex ? hex : HP_FAMILY_COLOR) + 1, NULL, 16));
    }
}

// --- clock, idle, sleep --------------------------------------------------------------------------

static bool in_sleep_window(void) {
    int off = s_cfg->sleep_off_min, on = s_cfg->sleep_on_min;
    if (off == on) return false;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    int m = tm.tm_hour * 60 + tm.tm_min;
    return off < on ? (m >= off && m < on) : (m >= off || m < on);
}

static void on_wake_touch(lv_event_t *e) {
    (void)e;  // swallow the first touch at night: it only wakes the screen
    lv_obj_delete(s_wake_catcher);
    s_wake_catcher = NULL;
}

static void update_sleep(uint32_t idle_ms) {
    bool off = in_sleep_window() && idle_ms > (uint32_t)s_cfg->wake_minutes * 60000u;
    if (off == s_screen_off) return;
    s_screen_off = off;
    board_set_backlight(off ? 0 : 80);
    if (off && !s_wake_catcher) {
        s_wake_catcher = box(lv_layer_top());
        lv_obj_set_size(s_wake_catcher, BOARD_LCD_H_RES, BOARD_LCD_V_RES);
        lv_obj_add_flag(s_wake_catcher, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(s_wake_catcher, on_wake_touch, LV_EVENT_PRESSED, NULL);
    }
}

static void tick(lv_timer_t *t) {
    (void)t;
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    char buf[48];
    if (s_use_12h) strftime(buf, sizeof buf, "%I:%M %p", &tm);
    else strftime(buf, sizeof buf, "%H:%M", &tm);
    lv_label_set_text(s_clock, buf[0] == '0' ? buf + 1 : buf);
    strftime(buf, sizeof buf, "%A %d %B", &tm);
    lv_label_set_text(s_date, buf);

    uint32_t idle = lv_display_get_inactive_time(NULL);
    if (s_filter_on && idle > FILTER_RESET_MS) {
        s_filter_on = false;
        render_legend();
        render_days();
    }
    hp_date_t this_week = hp_week_start(today());
    if (!s_follow_today && idle > IDLE_BACK_TO_THIS_WEEK_MS) set_week(this_week);
    else if (s_follow_today && (hp_date_cmp(s_week, this_week) != 0 || hp_date_cmp(s_rendered_today, today()) != 0))
        set_week(this_week);  // midnight / new week
    update_sleep(idle);
}

// --- screen ----------------------------------------------------------------------------------------

static lv_obj_t *nav_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb) {
    lv_obj_t *b = lv_button_create(parent);
    lv_obj_set_style_bg_color(b, lv_color_hex(C_CARD), 0);
    lv_obj_set_style_border_color(b, lv_color_hex(C_LINE), 0);
    lv_obj_set_style_border_width(b, 1, 0);
    lv_obj_set_style_radius(b, 10, 0);
    lv_obj_set_height(b, 52);
    lv_obj_set_style_pad_hor(b, 20, 0);
    lv_obj_t *l = label(b, text, F24, C_FG);
    lv_obj_center(l);
    if (cb) lv_obj_add_event_cb(b, cb, LV_EVENT_CLICKED, NULL);
    return b;
}

void ui_week_show(const hp_settings_t *settings) {
    s_cfg = settings;
    s_use_12h = strncmp(settings->cal.timezone, "America/", 8) == 0;
    s_week = hp_week_start(today());
    s_follow_today = true;
    s_filter_on = false;

    lv_obj_t *scr = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(scr, on_gesture, LV_EVENT_GESTURE, NULL);

    s_clock = label(scr, "", F48, C_FG);
    lv_obj_set_pos(s_clock, 16, 6);
    s_date = label(scr, "", F18, C_MUTED);
    lv_obj_set_pos(s_date, 18, 64);
    s_now_weather = label(scr, "", F24, C_FG);
    lv_obj_set_pos(s_now_weather, 300, 22);

    lv_obj_t *nav = box(scr);
    lv_obj_set_size(nav, LV_SIZE_CONTENT, 56);
    lv_obj_set_flex_flow(nav, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(nav, 8, 0);
    lv_obj_align(nav, LV_ALIGN_TOP_RIGHT, -12, 10);
    nav_button(nav, LV_SYMBOL_LEFT, on_prev);
    nav_button(nav, "Today", on_today);
    nav_button(nav, LV_SYMBOL_RIGHT, on_next);
    lv_obj_t *add = nav_button(nav, LV_SYMBOL_PLUS " Add", on_add);
    lv_obj_set_style_bg_color(add, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_text_color(lv_obj_get_child(add, 0), lv_color_hex(0x0b1220), 0);
    nav_button(nav, LV_SYMBOL_SETTINGS, ui_open_settings_menu);

    s_range = label(scr, "", F18, C_MUTED);
    lv_obj_set_pos(s_range, 18, 94);

    s_days = box(scr);
    lv_obj_set_size(s_days, 7 * COL_W + 6 * COL_GAP, DAYS_H);
    lv_obj_set_pos(s_days, 12, DAYS_Y);
    lv_obj_add_flag(s_days, LV_OBJ_FLAG_GESTURE_BUBBLE);

    s_legend = box(scr);
    // The legend ends where the sync status (bottom right, tappable) begins: no overlapping taps.
    lv_obj_set_size(s_legend, BOARD_LCD_H_RES - 12 - SYNC_W - SYNC_RIGHT - 2 * SYNC_TAP - 12, 40);
    lv_obj_set_scroll_dir(s_legend, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_legend, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_pos(s_legend, 12, DAYS_Y + DAYS_H + 10);
    lv_obj_set_flex_flow(s_legend, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(s_legend, 6, 0);

    s_synced = label(scr, "Syncing...", F18, C_MUTED);
    lv_obj_set_width(s_synced, SYNC_W);
    lv_obj_set_style_text_align(s_synced, LV_TEXT_ALIGN_RIGHT, 0);
    lv_label_set_long_mode(s_synced, LV_LABEL_LONG_DOT);
    lv_obj_align(s_synced, LV_ALIGN_BOTTOM_RIGHT, -SYNC_RIGHT, -16);
    lv_obj_add_flag(s_synced, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_ext_click_area(s_synced, SYNC_TAP);
    lv_obj_add_event_cb(s_synced, on_refresh, LV_EVENT_CLICKED, NULL);

    render_legend();
    render_days();
    render_status();
    lv_timer_create(tick, 1000, NULL);
    tick(NULL);
    lv_screen_load_anim(scr, LV_SCR_LOAD_ANIM_NONE, 0, 0, true);
}

void ui_week_update(void) {
    if (!s_days) return;
    if (s_cals_version != model_calendars_version()) {  // a calendar was added, renamed or removed
        if (s_filter_on && s_filter[0] == '\x02') s_filter_on = false;
        render_legend();
    }
    render_days();
    render_status();
}
