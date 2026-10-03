// See hp_edit.h. Mirrors the Pi version's sync.py edit logic.
#include "hp_edit.h"

#include <stdio.h>
#include <string.h>

#define GONE "This event no longer exists. It may have been deleted on another device."
#define GUEST_EDIT \
    "This event was created by someone else. Only they can change its title, time or place; you can change who it's for."
#define GUEST_DELETE "This event was created by someone else. Only they can delete it."

static hp_edit_result_t fail(hp_edit_result_t r, const char *text, char *msg, size_t size) {
    if (msg && size) {
        if (!text) {
            text = r == HP_EDIT_NOT_FOUND     ? GONE
                   : r == HP_EDIT_NETWORK     ? "Can't reach Google Calendar. Check the Wi-Fi and try again."
                   : r == HP_EDIT_NOT_ALLOWED ? "Google didn't allow this change. The event was probably created by someone else."
                                              : "Google Calendar couldn't save the change.";
        }
        snprintf(msg, size, "%s", text);
    }
    return r;
}

static const char *str_item(const cJSON *o, const char *key) {
    const cJSON *i = cJSON_GetObjectItemCaseSensitive(o, key);
    return cJSON_IsString(i) ? i->valuestring : NULL;
}

hp_edit_result_t hp_edit_details(const hp_calendar_ops_t *ops, const hp_config_t *cfg, const char *event_id,
                                 hp_event_details_t *out, char *msg, size_t msg_size) {
    cJSON *ev = NULL, *series = NULL;
    hp_edit_result_t r = ops->get(ops->ctx, event_id, &ev);
    if (r != HP_EDIT_OK) return fail(r, NULL, msg, msg_size);
    const char *series_id = str_item(ev, "recurringEventId");
    if (series_id) ops->get(ops->ctx, series_id, &series);  // missing series: fall back to the event's own rule
    bool ok = hp_event_to_form(ev, series, cfg, out);
    cJSON_Delete(ev);
    cJSON_Delete(series);
    return ok ? HP_EDIT_OK : fail(HP_EDIT_FAILED, "This event couldn't be read.", msg, msg_size);
}

hp_edit_result_t hp_edit_add(const hp_calendar_ops_t *ops, const hp_config_t *cfg, const hp_form_t *form, char *msg,
                             size_t msg_size) {
    const char *problem = hp_form_validate(form);
    if (problem) return fail(HP_EDIT_INVALID, problem, msg, msg_size);
    const char *err = NULL;
    cJSON *body = hp_form_to_google(form, cfg, &err);
    if (!body) return fail(HP_EDIT_INVALID, err, msg, msg_size);
    hp_edit_result_t r = ops->insert(ops->ctx, body, NULL);
    cJSON_Delete(body);
    return r == HP_EDIT_OK ? r : fail(r, NULL, msg, msg_size);
}

// Notes are shown and edited as plain text; if they weren't changed, send the original
// (possibly HTML) notes back so formatting made in Google Calendar isn't lost.
static void keep_untouched_notes(cJSON *body, const cJSON *original, const hp_form_t *form) {
    const char *html = str_item(original, "description");
    if (!html) return;
    char plain[HP_TEXT_LEN];
    hp_html_to_text(html, plain, sizeof plain);
    if (strcmp(plain, form->description) != 0) return;
    cJSON_DeleteItemFromObjectCaseSensitive(body, "description");
    cJSON_AddStringToObject(body, "description", html);
}

hp_edit_result_t hp_edit_update(const hp_calendar_ops_t *ops, const hp_config_t *cfg, const char *event_id,
                                const hp_form_t *form, bool all_events, char *msg, size_t msg_size) {
    if (msg && msg_size) msg[0] = '\0';
    const char *problem = hp_form_validate(form);
    if (problem) return fail(HP_EDIT_INVALID, problem, msg, msg_size);
    cJSON *event = NULL, *series = NULL, *body = NULL;
    hp_edit_result_t r = ops->get(ops->ctx, event_id, &event);
    if (r != HP_EDIT_OK) return fail(r, NULL, msg, msg_size);

    const char *series_id = str_item(event, "recurringEventId");
    if (series_id && all_events) {
        r = ops->get(ops->ctx, series_id, &series);
        if (r != HP_EDIT_OK) goto done;
    }
    const char *target_id = series ? series_id : event_id;
    const cJSON *target = series ? series : event;
    const char *err = NULL;

    if (!hp_is_owned(target, cfg)) {
        // The Family calendar is only a guest: Google lets it change just its own color.
        hp_event_details_t current;
        hp_event_to_form(event, NULL, cfg, &current);
        if (hp_guest_changes(&current.form, form)) {
            r = fail(HP_EDIT_NOT_ALLOWED, GUEST_EDIT, msg, msg_size);
            goto done;
        }
        const char *color = form->member[0] ? hp_color_for_member(cfg, form->member) : NULL;
        if (form->member[0] && !color) {
            r = fail(HP_EDIT_INVALID, "Unknown family member.", msg, msg_size);
            goto done;
        }
        body = cJSON_CreateObject();
        if (color) cJSON_AddStringToObject(body, "colorId", color);
        else cJSON_AddNullToObject(body, "colorId");
        r = ops->patch(ops->ctx, target_id, body, NULL);
    } else if (series) {
        // Moving the clicked occurrence by N days moves the whole series by N days.
        hp_date_t series_date = hp_occurrence_date(series);
        hp_form_t shifted = *form;
        shifted.date = hp_date_add(series_date, hp_date_diff(form->date, hp_occurrence_date(event)));
        hp_repeat_t rep;
        bool has_until;
        hp_date_t until;
        bool custom = !hp_parse_simple_rrule(cJSON_GetObjectItemCaseSensitive(series, "recurrence"), series_date, &rep,
                                             &has_until, &until);
        body = hp_apply_form(series, &shifted, cfg, custom, hp_all_day_span(series), &err);
        if (body) keep_untouched_notes(body, series, form);
        r = body ? ops->update(ops->ctx, series_id, body, NULL) : fail(HP_EDIT_INVALID, err, msg, msg_size);
    } else {
        bool keep = series_id != NULL;  // a single occurrence: its repeat belongs to the series
        const cJSON *rec = cJSON_GetObjectItemCaseSensitive(event, "recurrence");
        if (!keep && cJSON_IsArray(rec)) {
            hp_repeat_t rep;
            bool has_until;
            hp_date_t until;
            keep = !hp_parse_simple_rrule(rec, hp_occurrence_date(event), &rep, &has_until, &until);
        }
        body = hp_apply_form(event, form, cfg, keep, hp_all_day_span(event), &err);
        if (body) keep_untouched_notes(body, event, form);
        r = body ? ops->update(ops->ctx, event_id, body, NULL) : fail(HP_EDIT_INVALID, err, msg, msg_size);
    }
done:
    // A specific message (guest event, invalid form) may already be set; otherwise use the default.
    if (r != HP_EDIT_OK && msg && msg_size && !msg[0]) fail(r, NULL, msg, msg_size);
    cJSON_Delete(body);
    cJSON_Delete(event);
    cJSON_Delete(series);
    return r;
}

hp_edit_result_t hp_edit_delete(const hp_calendar_ops_t *ops, const hp_config_t *cfg, const char *event_id,
                                bool all_events, char *msg, size_t msg_size) {
    cJSON *event = NULL;
    hp_edit_result_t r = ops->get(ops->ctx, event_id, &event);
    if (r != HP_EDIT_OK) return fail(r, NULL, msg, msg_size);
    if (!hp_is_owned(event, cfg)) {
        cJSON_Delete(event);
        return fail(HP_EDIT_NOT_ALLOWED, GUEST_DELETE, msg, msg_size);
    }
    const char *series_id = str_item(event, "recurringEventId");
    char id[HP_ID_LEN];
    snprintf(id, sizeof id, "%s", all_events && series_id ? series_id : event_id);
    cJSON_Delete(event);
    r = ops->remove(ops->ctx, id);
    return r == HP_EDIT_OK ? r : fail(r, NULL, msg, msg_size);
}
