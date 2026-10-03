// Over-the-air updates on the device: install from a GitHub release or from an upload on the
// settings page; a new version must confirm itself (update_mark_good) within 5 minutes or the
// panel goes back to the previous one. Only firmware signed with the same key is accepted
// (ESP-IDF checks the signature; see sdkconfig.signing).
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <time.h>

#include "esp_err.h"

typedef enum {
    UPDATE_IDLE,        // not checked yet
    UPDATE_CHECKING,
    UPDATE_UP_TO_DATE,
    UPDATE_AVAILABLE,   // latest/notes set
    UPDATE_INSTALLING,  // percent set; the panel restarts when done
    UPDATE_FAILED,      // message set
} update_state_t;

typedef struct {
    update_state_t state;
    int percent;
    char message[160];
    char current[32];
    char latest[32];
    char notes[640];
    time_t checked;  // last successful check, 0 = never
} update_status_t;

typedef void (*update_listener_t)(void);  // called from update tasks on any change

void update_boot(void);  // first thing at start-up (rollback timer, notices)
void update_mark_good(void);  // this version works (online): keep it
void update_set_listener(update_listener_t listener);
const char *update_current_version(void);
update_status_t update_status(void);

void update_start_checks(void);  // daily checks of GitHub (and the first one a minute after start)
void update_check_now(void);
esp_err_t update_install_available(char *err, size_t err_size);  // starts installing the available release

// One-time message after an update ("Updated to 1.2.0" or that it was undone). False if none.
bool update_take_notice(char *out, size_t size);

// Uploads (settings page / tools/ota.sh), streamed in pieces. end(true) installs and restarts.
esp_err_t update_upload_begin(size_t size, char *err, size_t err_size);
esp_err_t update_upload_write(const void *data, size_t len, char *err, size_t err_size);
esp_err_t update_upload_end(bool complete, char *err, size_t err_size);
