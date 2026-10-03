// Shared between the UI source files (not part of the public ui.h API).
#pragma once
#include <stdbool.h>

#include "hp_logic.h"

bool ui_use_12h(void);
void ui_toast(const char *text);  // LVGL lock held
void ui_form_open_add(hp_date_t date);
void ui_form_open_edit(const char *event_id, const hp_event_details_t *details, bool all_events);
