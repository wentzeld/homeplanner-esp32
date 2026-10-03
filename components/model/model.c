#include "model.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_heap_caps.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "gauth.h"
#include "gcal.h"
#include "ical.h"
#include "net.h"

static const char *TAG = "model";

#define SYNC_INTERVAL_MS (2 * 60 * 1000)
#define SYNC_RETRY_MS (15 * 1000)
#define WEATHER_INTERVAL_MS (30 * 60 * 1000)
#define WEATHER_RETRY_MS (60 * 1000)
#define DAYS_BACK 7
#define DAYS_AHEAD 35
#define EVENTS_FILE "/storage/events.json"
#define WEATHER_FILE "/storage/weather.bin"

static hp_settings_t s_settings;
static SemaphoreHandle_t s_lock;
static TaskHandle_t s_sync_task, s_weather_task;
static model_listener_t s_on_change;

static hp_event_t *s_events;  // PSRAM: the Family calendar
static int s_event_count;
static hp_event_t *s_all;     // PSRAM: Family + other calendars' occurrences (what the screen shows)
static int s_all_count;

#define EXT_REFRESH_MS (30 * 60 * 1000)
#define EXT_RETRY_MS (5 * 60 * 1000)
#define EXT_MAX_BYTES (8 * 1024 * 1024)
#define EXT_MAX_OCCURRENCES 3000

typedef struct {
    hp_calendar_t cfg;
    ical_cal_t *parsed;  // iCal: NULL until downloaded (or loaded from flash)
    hp_event_t *gevents; // Google: events in the window (PSRAM)
    int gcount;
    time_t last_updated;
    char error[160];
} ext_cal_t;

static ext_cal_t s_ext[HP_MAX_CALENDARS];
static int s_ext_count;
static unsigned s_ext_version;
static TaskHandle_t s_ext_task;
static weather_report_t s_weather;
static model_status_t s_status;
static hp_date_t s_window_start, s_window_end;  // [start, end) covered by s_events
static volatile bool s_extra_active;  // a week outside the window is on screen: keep it loaded too
static hp_date_t s_extra_week;

static SemaphoreHandle_t lock_handle(void) {
    static StaticSemaphore_t storage;
    if (!s_lock) s_lock = xSemaphoreCreateMutexStatic(&storage);  // safe even before model_start()
    return s_lock;
}

void model_lock(void) { xSemaphoreTake(lock_handle(), portMAX_DELAY); }
void model_unlock(void) { xSemaphoreGive(lock_handle()); }
const hp_event_t *model_events(int *count) {
    *count = s_all_count;
    return s_all;
}
const weather_report_t *model_weather(void) { return &s_weather; }
const hp_config_t *model_config(void) { return &s_settings.cal; }
model_status_t model_status(void) { return s_status; }

static void notify(void) {
    if (s_on_change) s_on_change();
}

// --- merging the other calendars in ---------------------------------------------------------------

typedef struct {
    hp_event_t *v;
    int n, cap;
    const ext_cal_t *cal;
    const char *color;
    int64_t skip_from, skip_to;  // occurrences overlapping this range were already added
} collect_t;

static bool collect(const ical_cal_t *cal, const ical_occurrence_t *occ, void *ctx) {
    collect_t *c = ctx;
    int64_t end = occ->end > occ->start ? occ->end : occ->start + 1;
    if (occ->start < c->skip_to && end > c->skip_from) return true;
    if (c->n >= EXT_MAX_OCCURRENCES) return false;
    if (c->n == c->cap) {
        int cap = c->cap ? c->cap * 2 : 256;
        hp_event_t *v = heap_caps_realloc(c->v, (size_t)cap * sizeof *v, MALLOC_CAP_SPIRAM);
        if (!v) return false;
        c->v = v, c->cap = cap;
    }
    ical_occurrence_to_event(cal, occ, c->cal->cfg.id, c->cal->cfg.name, c->color, &c->v[c->n++]);
    return true;
}

// s_all = Family events + other calendars expanded over the synced window (and an extra week
// being looked at). With the lock held.
static void rebuild_all(void) {
    hp_date_t today = hp_local_date(time(NULL));
    hp_date_t from = hp_date_add(today, -DAYS_BACK);
    collect_t c = {.skip_from = 1, .skip_to = 0};
    for (int i = 0; i < s_ext_count; i++) {
        for (int k = 0; k < s_ext[i].gcount && c.n < EXT_MAX_OCCURRENCES; k++) {
            if (c.n == c.cap) {
                int cap = c.cap ? c.cap * 2 : 256;
                hp_event_t *v = heap_caps_realloc(c.v, (size_t)cap * sizeof *v, MALLOC_CAP_SPIRAM);
                if (!v) break;
                c.v = v, c.cap = cap;
            }
            c.v[c.n++] = s_ext[i].gevents[k];
        }
        if (!s_ext[i].parsed) continue;
        c.cal = &s_ext[i];
        c.color = hp_palette_hex(s_ext[i].cfg.color);
        c.skip_from = 1, c.skip_to = 0;
        ical_expand(s_ext[i].parsed, s_settings.tz_posix, from, DAYS_BACK + DAYS_AHEAD, collect, &c);
        if (s_extra_active) {
            c.skip_from = hp_local_midnight(from);
            c.skip_to = hp_local_midnight(hp_date_add(today, DAYS_AHEAD));
            ical_expand(s_ext[i].parsed, s_settings.tz_posix, s_extra_week, 7, collect, &c);
        }
    }
    hp_event_t *all = heap_caps_calloc((size_t)(s_event_count + c.n + 1), sizeof(hp_event_t), MALLOC_CAP_SPIRAM);
    if (all) {
        if (s_event_count) memcpy(all, s_events, sizeof(hp_event_t) * s_event_count);
        if (c.n) memcpy(all + s_event_count, c.v, sizeof(hp_event_t) * c.n);
        free(s_all);
        s_all = all;
        s_all_count = s_event_count + c.n;
    }
    free(c.v);
}

// Convert Google items to display events; keeps `keep` existing events outside [from, to).
static void replace_events(const cJSON *items, hp_date_t from, hp_date_t to, bool merge) {
    int n = cJSON_GetArraySize(items);
    int64_t t_from = hp_local_midnight(from), t_to = hp_local_midnight(to);
    int kept = 0;
    hp_event_t *next = heap_caps_calloc((merge ? s_event_count : 0) + n + 1, sizeof(hp_event_t), MALLOC_CAP_SPIRAM);
    if (!next) return;
    if (merge)  // keep events that don't overlap the refreshed range
        for (int i = 0; i < s_event_count; i++)
            if (s_events[i].end <= t_from || s_events[i].start >= t_to) next[kept++] = s_events[i];
    const cJSON *item;
    cJSON_ArrayForEach(item, items) {
        if (hp_event_from_google(item, &s_settings.cal, &next[kept])) kept++;
    }
    free(s_events);
    s_events = next;
    s_event_count = kept;
    rebuild_all();
}

static void save_events_cache(const cJSON *items) {
    char *text = cJSON_PrintUnformatted(items);
    FILE *f = text ? fopen(EVENTS_FILE ".tmp", "w") : NULL;
    if (f) {
        fputs(text, f);
        fclose(f);
        rename(EVENTS_FILE ".tmp", EVENTS_FILE);
    }
    free(text);
}

static void load_cache(void) {
    FILE *f = fopen(EVENTS_FILE, "r");
    if (f) {
        fseek(f, 0, SEEK_END);
        long size = ftell(f);
        fseek(f, 0, SEEK_SET);
        char *text = heap_caps_malloc(size + 1, MALLOC_CAP_SPIRAM);
        if (text && fread(text, 1, size, f) == (size_t)size) {
            text[size] = '\0';
            cJSON *items = cJSON_Parse(text);
            if (cJSON_IsArray(items)) {
                replace_events(items, (hp_date_t){1970, 1, 1}, (hp_date_t){1970, 1, 1}, false);
                ESP_LOGI(TAG, "loaded %d cached events", s_event_count);
            }
            cJSON_Delete(items);
        }
        free(text);
        fclose(f);
    }
    f = fopen(WEATHER_FILE, "rb");
    if (f) {
        if (fread(&s_weather, sizeof s_weather, 1, f) != 1) memset(&s_weather, 0, sizeof s_weather);
        fclose(f);
    }
}

static bool sync_range(hp_date_t from, hp_date_t to, bool is_window) {
    cJSON *items = NULL;
    char err[200];
    model_lock();
    s_status.syncing = true;
    model_unlock();
    esp_err_t e = gcal_list(s_settings.cal.calendar_id, hp_local_midnight(from), hp_local_midnight(to), &items, err, sizeof err);
    model_lock();
    s_status.syncing = false;
    if (e == ESP_OK) {
        replace_events(items, from, to, !is_window);
        if (is_window) {
            s_window_start = from, s_window_end = to;
            s_status.last_synced = time(NULL);
            s_status.error = s_status.network_error = false;
            s_status.message[0] = '\0';
        }
    } else if (is_window) {
        s_status.error = true;
        s_status.network_error = e == ESP_ERR_TIMEOUT;
        snprintf(s_status.message, sizeof s_status.message, "%s", err);
    }
    model_unlock();
    if (e == ESP_OK && is_window) save_events_cache(items);
    cJSON_Delete(items);
    notify();
    return e == ESP_OK;
}

static void sync_task(void *arg) {
    (void)arg;
    for (;;) {
        hp_date_t today = hp_local_date(time(NULL));
        bool ok = sync_range(hp_date_add(today, -DAYS_BACK), hp_date_add(today, DAYS_AHEAD), true);
        if (s_extra_active) sync_range(s_extra_week, hp_date_add(s_extra_week, 7), false);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ok ? SYNC_INTERVAL_MS : SYNC_RETRY_MS));
    }
}

static void weather_task(void *arg) {
    (void)arg;
    for (;;) {
        bool ok = false;
        if (s_settings.has_coords) {
            weather_report_t r;
            ok = weather_fetch(s_settings.latitude, s_settings.longitude, s_settings.fahrenheit, s_settings.cal.timezone, &r) == ESP_OK;
            if (ok) {
                model_lock();
                s_weather = r;
                model_unlock();
                FILE *f = fopen(WEATHER_FILE, "wb");
                if (f) {
                    fwrite(&r, sizeof r, 1, f);
                    fclose(f);
                }
                notify();
            }
        }
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ok ? WEATHER_INTERVAL_MS : WEATHER_RETRY_MS));
    }
}

void model_want_week(hp_date_t sunday) {
    bool inside = hp_date_cmp(sunday, s_window_start) >= 0 && hp_date_cmp(hp_date_add(sunday, 7), s_window_end) <= 0;
    if (inside) {
        s_extra_active = false;
        return;
    }
    if (s_extra_active && hp_date_cmp(s_extra_week, sunday) == 0) return;
    s_extra_week = sunday;
    s_extra_active = true;
    if (s_sync_task) xTaskNotifyGive(s_sync_task);  // fetch it now
}

void model_refresh_now(void) {
    if (s_sync_task) xTaskNotifyGive(s_sync_task);
    if (s_weather_task) xTaskNotifyGive(s_weather_task);
}

// --- other calendars: list, flash copies, downloads ---------------------------------------------------

static void ics_path(const char *id, char *out, size_t size) { snprintf(out, size, "/storage/ext_%s.ics", id); }

static void save_ics(const char *id, const char *ics, size_t len) {
    char path[48], tmp[52];
    ics_path(id, path, sizeof path);
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    bool ok = f && fwrite(ics, 1, len, f) == len;
    if (f) fclose(f);
    if (ok) rename(tmp, path);
    else {
        remove(tmp);
        ESP_LOGW(TAG, "couldn't keep a copy of calendar %s on flash", id);
    }
}

static char *load_ics(const char *id, size_t *len) {
    char path[48];
    ics_path(id, path, sizeof path);
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = size > 0 ? heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM) : NULL;
    if (text && fread(text, 1, (size_t)size, f) == (size_t)size) {
        text[size] = '\0';
        *len = (size_t)size;
    } else {
        free(text);
        text = NULL;
    }
    fclose(f);
    return text;
}

static ext_cal_t *find_ext(const char *id) {  // lock held
    for (int i = 0; i < s_ext_count; i++)
        if (strcmp(s_ext[i].cfg.id, id) == 0) return &s_ext[i];
    return NULL;
}

// Parse a feed and put it in place (unless the calendar was removed meanwhile).
static void install(const char *id, const char *ics, size_t len, bool downloaded) {
    ical_status_t st;
    ical_cal_t *parsed = ical_parse(ics, len, &st);
    model_lock();
    ext_cal_t *e = find_ext(id);
    if (e && parsed) {
        ical_free(e->parsed);
        e->parsed = parsed;
        parsed = NULL;
        if (downloaded) {
            e->last_updated = time(NULL);
            e->error[0] = '\0';
        }
        if (ical_unsupported_count(e->parsed))
            ESP_LOGI(TAG, "\"%s\": %d repeating events use rules shown as their first date only", e->cfg.name,
                     ical_unsupported_count(e->parsed));
        rebuild_all();
    } else if (e && downloaded) {
        snprintf(e->error, sizeof e->error, "%s", st == ICAL_NO_MEMORY ? "Too large for the panel." : "The link no longer returns a calendar.");
    }
    model_unlock();
    ical_free(parsed);
}

// Google calendar items -> read-only display events for calendar id.
static void install_google(const char *id, const cJSON *items, bool downloaded) {
    int n = cJSON_GetArraySize(items);
    hp_event_t *v = heap_caps_calloc((size_t)n + 1, sizeof *v, MALLOC_CAP_SPIRAM);
    if (!v) return;
    model_lock();
    ext_cal_t *e = find_ext(id);
    int kept = 0;
    if (e) {
        const char *hex = hp_palette_hex(e->cfg.color);
        const cJSON *item;
        cJSON_ArrayForEach(item, items) {
            hp_event_t *ev = &v[kept];
            if (!hp_event_from_google(item, &s_settings.cal, ev)) continue;
            ev->member[0] = '\0';  // event colors mean nothing here
            snprintf(ev->color, sizeof ev->color, "%s", hex ? hex : HP_FAMILY_COLOR);
            snprintf(ev->calendar, sizeof ev->calendar, "%s", e->cfg.name);
            ev->editable = ev->owned = false;
            kept++;
        }
        free(e->gevents);
        e->gevents = v, e->gcount = kept;
        v = NULL;
        if (downloaded) {
            e->last_updated = time(NULL);
            e->error[0] = '\0';
        }
        rebuild_all();
    }
    model_unlock();
    free(v);
}

static void fetch_google(const hp_calendar_t *cal) {
    hp_date_t today = hp_local_date(time(NULL));
    cJSON *items = NULL;
    char err[160];
    esp_err_t e = gcal_list(cal->google_id, hp_local_midnight(hp_date_add(today, -DAYS_BACK)),
                            hp_local_midnight(hp_date_add(today, 60)), &items, err, sizeof err);
    if (e == ESP_OK) {
        install_google(cal->id, items, true);
        char path[48];
        snprintf(path, sizeof path, "/storage/gcal_%s.json", cal->id);
        char *text = cJSON_PrintUnformatted(items);
        FILE *f = text ? fopen(path, "w") : NULL;
        if (f) {
            fputs(text, f);
            fclose(f);
        }
        free(text);
        ESP_LOGI(TAG, "Google calendar \"%s\" updated (%d events)", cal->name, cJSON_GetArraySize(items));
    } else {
        model_lock();
        ext_cal_t *x = find_ext(cal->id);
        if (x) snprintf(x->error, sizeof x->error, "%s", err);
        model_unlock();
    }
    cJSON_Delete(items);
}

static void load_google_cache(const hp_calendar_t *cal) {
    char path[48];
    snprintf(path, sizeof path, "/storage/gcal_%s.json", cal->id);
    FILE *f = fopen(path, "r");
    if (!f) return;
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = size > 0 ? heap_caps_malloc((size_t)size + 1, MALLOC_CAP_SPIRAM) : NULL;
    if (text && fread(text, 1, (size_t)size, f) == (size_t)size) {
        text[size] = '\0';
        cJSON *items = cJSON_Parse(text);
        if (cJSON_IsArray(items)) install_google(cal->id, items, false);
        cJSON_Delete(items);
    }
    free(text);
    fclose(f);
}

// Reload the list from settings, keeping what's loaded for calendars that stay.
static void reload_list(void) {
    // Heap, not stack: these are ~11 KB each and this runs on the web server's task too.
    hp_calendars_t *list = calloc(1, sizeof *list);
    ext_cal_t *old = heap_caps_calloc(HP_MAX_CALENDARS, sizeof *old, MALLOC_CAP_SPIRAM);
    if (!list || !old) {
        free(list);
        free(old);
        return;
    }
    calendars_load(list);
    model_lock();
    int old_count = s_ext_count;
    memcpy(old, s_ext, sizeof s_ext);
    memset(s_ext, 0, sizeof s_ext);
    s_ext_count = 0;
    for (int i = 0; i < list->count; i++) {
        ext_cal_t *e = &s_ext[s_ext_count++];
        e->cfg = list->items[i];
        for (int j = 0; j < old_count; j++) {
            if (strcmp(old[j].cfg.id, e->cfg.id) != 0) continue;
            e->parsed = old[j].parsed, e->last_updated = old[j].last_updated;
            e->gevents = old[j].gevents, e->gcount = old[j].gcount;
            for (int k = 0; k < e->gcount; k++) {  // renamed / recolored
                snprintf(e->gevents[k].calendar, sizeof e->gevents[k].calendar, "%s", e->cfg.name);
                const char *hex = hp_palette_hex(e->cfg.color);
                snprintf(e->gevents[k].color, sizeof e->gevents[k].color, "%s", hex ? hex : HP_FAMILY_COLOR);
            }
            snprintf(e->error, sizeof e->error, "%s", old[j].error);
            old[j].parsed = NULL;
            old[j].gevents = NULL;
        }
    }
    s_ext_version++;
    rebuild_all();
    model_unlock();
    for (int j = 0; j < old_count; j++) {
        if (calendars_find(list, old[j].cfg.id)) continue;
        char path[48];
        ics_path(old[j].cfg.id, path, sizeof path);
        remove(path);  // removed calendar: drop its copy
        snprintf(path, sizeof path, "/storage/gcal_%s.json", old[j].cfg.id);
        remove(path);
        ical_free(old[j].parsed);
        free(old[j].gevents);
    }
    free(old);
    free(list);
}

static void ext_task(void *arg) {
    (void)arg;
    // First the copies on flash (works offline), then fresh downloads.
    hp_calendar_t *ids = heap_caps_calloc(HP_MAX_CALENDARS, sizeof *ids, MALLOC_CAP_SPIRAM);
    if (!ids) {
        vTaskDelete(NULL);
        return;
    }
    int n = model_calendars(ids, HP_MAX_CALENDARS);
    for (int i = 0; i < n; i++) {
        if (ids[i].google) {
            load_google_cache(&ids[i]);
            continue;
        }
        size_t len = 0;
        char *ics = load_ics(ids[i].id, &len);
        if (ics) install(ids[i].id, ics, len, false);
        free(ics);
    }
    if (n) notify();
    for (;;) {
        bool failing = false;
        n = model_calendars(ids, HP_MAX_CALENDARS);
        for (int i = 0; i < n; i++) {
            if (ids[i].google) {
                fetch_google(&ids[i]);
            } else {
            char *ics = NULL, err[160];
            size_t len = 0;
            if (net_download(ids[i].url, EXT_MAX_BYTES, &ics, &len, err, sizeof err) == ESP_OK) {
                install(ids[i].id, ics, len, true);
                save_ics(ids[i].id, ics, len);
                ESP_LOGI(TAG, "calendar \"%s\" updated (%u bytes)", ids[i].name, (unsigned)len);
            } else {
                model_lock();
                ext_cal_t *e = find_ext(ids[i].id);
                if (e) snprintf(e->error, sizeof e->error, "%s", err);
                model_unlock();
            }
            free(ics);
            }
            model_lock();
            ext_cal_t *e = find_ext(ids[i].id);
            failing |= e && e->error[0];
            model_unlock();
        }
        if (n) notify();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(failing ? EXT_RETRY_MS : EXT_REFRESH_MS));
    }
}

void model_calendar_added(const hp_calendar_t *cal, char *ics, size_t len) {
    reload_list();  // the settings page saved it already
    if (ics) {
        install(cal->id, ics, len, true);
        save_ics(cal->id, ics, len);
        free(ics);
    }
    if (cal->google && s_ext_task) xTaskNotifyGive(s_ext_task);  // fetch it from Google now
    notify();
}

void model_calendars_changed(void) {
    reload_list();
    notify();
}

bool model_calendar_status(const char *id, time_t *last_updated, char *error, size_t error_size) {
    model_lock();
    ext_cal_t *e = find_ext(id);
    if (e) {
        *last_updated = e->last_updated;
        snprintf(error, error_size, "%s", e->error);
    }
    model_unlock();
    return e != NULL;
}

int model_calendars(hp_calendar_t *out, int max) {
    model_lock();
    int n = s_ext_count < max ? s_ext_count : max;
    for (int i = 0; i < n; i++) out[i] = s_ext[i].cfg;
    model_unlock();
    return n;
}

unsigned model_calendars_version(void) { return s_ext_version; }

void model_start(const hp_settings_t *settings, model_listener_t on_change) {
    s_settings = *settings;
    s_on_change = on_change;
    lock_handle();
    esp_vfs_littlefs_conf_t fs = {.base_path = "/storage", .partition_label = "storage", .format_if_mount_failed = true};
    if (esp_vfs_littlefs_register(&fs) != ESP_OK) ESP_LOGW(TAG, "storage not mounted; no offline cache");
    reload_list();
    load_cache();
    xTaskCreate(sync_task, "sync", 12 * 1024, NULL, 5, &s_sync_task);
    xTaskCreate(weather_task, "weather", 8 * 1024, NULL, 4, &s_weather_task);
    xTaskCreate(ext_task, "calendars", 16 * 1024, NULL, 4, &s_ext_task);
}

// --- edits ------------------------------------------------------------------------------------------

typedef enum { JOB_DETAILS, JOB_ADD, JOB_UPDATE, JOB_DELETE } job_kind_t;
typedef struct {
    job_kind_t kind;
    char event_id[HP_ID_LEN];
    hp_form_t form;
    bool all_events;
    model_edit_cb_t cb;
} job_t;

static void edit_task(void *arg) {
    job_t *job = arg;
    hp_calendar_ops_t ops = gcal_ops(s_settings.cal.calendar_id);
    char msg[200] = "";
    hp_event_details_t details = {0};
    hp_edit_result_t r = HP_EDIT_FAILED;
    switch (job->kind) {
        case JOB_DETAILS: r = hp_edit_details(&ops, &s_settings.cal, job->event_id, &details, msg, sizeof msg); break;
        case JOB_ADD: r = hp_edit_add(&ops, &s_settings.cal, &job->form, msg, sizeof msg); break;
        case JOB_UPDATE:
            r = hp_edit_update(&ops, &s_settings.cal, job->event_id, &job->form, job->all_events, msg, sizeof msg);
            break;
        case JOB_DELETE: r = hp_edit_delete(&ops, &s_settings.cal, job->event_id, job->all_events, msg, sizeof msg); break;
    }
    ESP_LOGI(TAG, "edit job %d -> %d %s", job->kind, r, msg);
    if (job->cb) job->cb(r, msg, &details);
    if (job->kind != JOB_DETAILS && (r == HP_EDIT_OK || r == HP_EDIT_NOT_FOUND)) model_refresh_now();
    free(job);
    vTaskDelete(NULL);
}

static void start_job(job_kind_t kind, const char *event_id, const hp_form_t *form, bool all, model_edit_cb_t cb) {
    job_t *job = heap_caps_calloc(1, sizeof *job, MALLOC_CAP_SPIRAM);
    if (!job) {
        if (cb) cb(HP_EDIT_FAILED, "Out of memory.", NULL);
        return;
    }
    job->kind = kind;
    if (event_id) snprintf(job->event_id, sizeof job->event_id, "%s", event_id);
    if (form) job->form = *form;
    job->all_events = all;
    job->cb = cb;
    if (xTaskCreate(edit_task, "edit", 16 * 1024, job, 5, NULL) != pdPASS) {
        free(job);
        if (cb) cb(HP_EDIT_FAILED, "Busy, please try again.", NULL);
    }
}

void model_edit_details(const char *event_id, model_edit_cb_t cb) { start_job(JOB_DETAILS, event_id, NULL, false, cb); }
void model_edit_add(const hp_form_t *form, model_edit_cb_t cb) { start_job(JOB_ADD, NULL, form, false, cb); }
void model_edit_update(const char *event_id, const hp_form_t *form, bool all, model_edit_cb_t cb) {
    start_job(JOB_UPDATE, event_id, form, all, cb);
}
void model_edit_delete(const char *event_id, bool all, model_edit_cb_t cb) { start_job(JOB_DELETE, event_id, NULL, all, cb); }
