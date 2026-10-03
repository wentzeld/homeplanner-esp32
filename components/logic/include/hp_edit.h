// Adding, editing and deleting Family-calendar events: the same rules as the Raspberry Pi
// version's sync.update_event / delete_event, with the Google calls injected (host-tested).
#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "cJSON.h"
#include "hp_logic.h"

typedef enum {
    HP_EDIT_OK,
    HP_EDIT_NOT_FOUND,    // deleted elsewhere in the meantime
    HP_EDIT_NOT_ALLOWED,  // e.g. the Family calendar is only a guest of the event
    HP_EDIT_INVALID,      // form problem (message says what)
    HP_EDIT_NETWORK,      // Google unreachable
    HP_EDIT_FAILED,
} hp_edit_result_t;

// Google Calendar calls. Returned objects (*out) are owned by the caller (may be NULL if unused).
typedef struct {
    void *ctx;
    hp_edit_result_t (*get)(void *ctx, const char *id, cJSON **out);
    hp_edit_result_t (*insert)(void *ctx, const cJSON *body, cJSON **out);
    hp_edit_result_t (*update)(void *ctx, const char *id, const cJSON *body, cJSON **out);
    hp_edit_result_t (*patch)(void *ctx, const char *id, const cJSON *body, cJSON **out);
    hp_edit_result_t (*remove)(void *ctx, const char *id);
} hp_calendar_ops_t;

// Pre-filled edit form for an event (its series' repeat rule for an occurrence).
hp_edit_result_t hp_edit_details(const hp_calendar_ops_t *ops, const hp_config_t *cfg, const char *event_id,
                                 hp_event_details_t *out, char *msg, size_t msg_size);

hp_edit_result_t hp_edit_add(const hp_calendar_ops_t *ops, const hp_config_t *cfg, const hp_form_t *form, char *msg,
                             size_t msg_size);

// all_events: change the whole series an occurrence belongs to (else just this event).
hp_edit_result_t hp_edit_update(const hp_calendar_ops_t *ops, const hp_config_t *cfg, const char *event_id,
                                const hp_form_t *form, bool all_events, char *msg, size_t msg_size);

hp_edit_result_t hp_edit_delete(const hp_calendar_ops_t *ops, const hp_config_t *cfg, const char *event_id,
                                bool all_events, char *msg, size_t msg_size);
