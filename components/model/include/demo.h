// Demo mode (CONFIG_HP_DEMO): a made-up family, events and weather instead of Google and Open-Meteo,
// for screenshots and trying the panel out. Nothing here touches the network or flash.
#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "hp_edit.h"
#include "settings.h"
#include "weather.h"

#define DEMO_CALENDARS 2

// The made-up family members (replaces cfg's members).
void demo_members(hp_config_t *cfg);

// The made-up other calendars ("School", "Soccer club").
void demo_calendars(hp_calendars_t *out);

// Family calendar events in Google's format (caller deletes). Built on first use around this week.
cJSON *demo_family_items(void);

// Events of other calendar i (0..DEMO_CALENDARS-1) in Google's format (caller deletes).
cJSON *demo_calendar_items(int i);

// Add/edit/delete on the made-up Family calendar (kept in memory only).
hp_calendar_ops_t demo_ops(void);

void demo_weather(bool fahrenheit, weather_report_t *out);
