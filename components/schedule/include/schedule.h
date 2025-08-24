#pragma once

#include <time.h>
#include <esp_err.h>
#include <stdbool.h>

// Schedule component: compute next ON/OFF events based on UTC time + IANA TZ string.
// Stores and retrieves schedules via storage component (blob API).

typedef struct {
    int on_hour;   // 0-23 local
    int on_min;    // 0-59
    int off_hour;  // 0-23 local
    int off_min;   // 0-59
    char tz[64];   // IANA timezone string, e.g., "America/Los_Angeles"
} schedule_t;

// Initialize schedule subsystem
esp_err_t schedule_init(void);

// Load saved schedule; if not present, fills with defaults and returns ESP_OK
esp_err_t schedule_load(schedule_t *out);

// Save schedule
esp_err_t schedule_save(const schedule_t *s);

// Compute next ON and OFF event (UTC timestamps) relative to now_utc. Returns ESP_OK if computed.
esp_err_t schedule_compute_next_events(time_t now_utc, const schedule_t *s, time_t *next_on_utc, time_t *next_off_utc);

// Reconcile missed events since last_boot_utc and perform callback for each missed event (callback runs in caller context)
// callback(event_is_on, event_time_utc, void *arg)
typedef void (*schedule_event_cb_t)(bool on, time_t event_time_utc, void *arg);
esp_err_t schedule_reconcile(time_t last_seen_utc, time_t now_utc, const schedule_t *s, schedule_event_cb_t cb, void *arg);
