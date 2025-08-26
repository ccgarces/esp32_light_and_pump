#include "schedule.h"
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_task_wdt.h"
#include "storage.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "ipc.h"

static const char *TAG = "schedule";

#define STORAGE_KEY_SCHEDULE "schedule_cfg"

// Forward declaration
static void schedule_task(void *arg);

esp_err_t schedule_init(void)
{
    // This function is called from app_main, which already initializes storage.
    // No need to call storage_init() here.

    // Load the schedule to apply timezone early.
    schedule_t s;
    if (schedule_load(&s) == ESP_OK && s.tz[0]) {
        setenv("TZ", s.tz, 1);
        tzset();
    }

    BaseType_t r = xTaskCreate(schedule_task, "schedule_task", 4096, NULL, 4, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create schedule_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "schedule initialized");
    return ESP_OK;
}

static void fill_defaults(schedule_t *s)
{
    s->on_hour = CONFIG_SCHEDULE_DEFAULT_ON_HOUR;
    s->on_min = 0;
    s->off_hour = CONFIG_SCHEDULE_DEFAULT_OFF_HOUR;
    s->off_min = 0;
    strncpy(s->tz, CONFIG_SCHEDULE_DEFAULT_TZ, sizeof(s->tz) - 1);
    s->tz[sizeof(s->tz) - 1] = '\0';
}

esp_err_t schedule_load(schedule_t *out)
{
    if (!out) return ESP_ERR_INVALID_ARG;

    size_t len = sizeof(schedule_t);
    esp_err_t err = storage_load_config(STORAGE_KEY_SCHEDULE, out, &len);

    if (err == ESP_OK && len == sizeof(schedule_t)) {
        ESP_LOGI(TAG, "Loaded schedule: ON %02d:%02d, OFF %02d:%02d, TZ=%s",
                 out->on_hour, out->on_min, out->off_hour, out->off_min, out->tz);
        return ESP_OK;
    }

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        ESP_LOGI(TAG, "No schedule found in NVS, using defaults.");
    } else {
        ESP_LOGW(TAG, "Failed to load schedule (err=%s, len=%d), using defaults.", esp_err_to_name(err), len);
    }

    fill_defaults(out);
    return schedule_save(out); // Save defaults back to NVS
}

esp_err_t schedule_save(const schedule_t *s)
{
    if (!s) return ESP_ERR_INVALID_ARG;
    esp_err_t err = storage_save_config(STORAGE_KEY_SCHEDULE, s, sizeof(*s));
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Saved schedule: ON %02d:%02d, OFF %02d:%02d, TZ=%s",
                 s->on_hour, s->on_min, s->off_hour, s->off_min, s->tz);
        // Apply the new timezone immediately
        setenv("TZ", s->tz, 1);
        tzset();
    } else {
        ESP_LOGE(TAG, "Failed to save schedule: %s", esp_err_to_name(err));
    }
    return err;
}

// Helper to build a time_t for the next occurrence of hour:min, starting from a given date.
// It correctly handles advancing to the next day if the time has already passed.
static time_t get_next_event_time(time_t now_utc, int hour, int min)
{
    struct tm local_tm;
    localtime_r(&now_utc, &local_tm);

    struct tm event_tm = local_tm;
    event_tm.tm_hour = hour;
    event_tm.tm_min = min;
    event_tm.tm_sec = 0;

    time_t event_utc = mktime(&event_tm);

    // If the calculated event time is in the past, calculate it for the next day.
    if (event_utc <= now_utc) {
        event_tm.tm_mday += 1;
        // mktime handles month/year rollovers
        event_utc = mktime(&event_tm);
    }
    return event_utc;
}

esp_err_t schedule_compute_next_events(time_t now_utc, const schedule_t *s, time_t *next_on_utc, time_t *next_off_utc)
{
    if (!s || !next_on_utc || !next_off_utc) return ESP_ERR_INVALID_ARG;

    *next_on_utc = get_next_event_time(now_utc, s->on_hour, s->on_min);
    *next_off_utc = get_next_event_time(now_utc, s->off_hour, s->off_min);

    return ESP_OK;
}

/**
 * @brief Determines if, according to the schedule, the lights should currently be ON.
 *
 * This function handles the case where the ON/OFF times span across midnight.
 * E.g., ON at 22:00, OFF at 06:00.
 */
static bool is_currently_on(time_t now_utc, const schedule_t *s)
{
    struct tm local_tm;
    localtime_r(&now_utc, &local_tm);

    int now_min_of_day = local_tm.tm_hour * 60 + local_tm.tm_min;
    int on_min_of_day = s->on_hour * 60 + s->on_min;
    int off_min_of_day = s->off_hour * 60 + s->off_min;

    if (on_min_of_day < off_min_of_day) {
        // Simple case: ON and OFF are on the same day.
        // e.g., ON 07:00, OFF 21:00
        return now_min_of_day >= on_min_of_day && now_min_of_day < off_min_of_day;
    } else {
        // Overnight case: ON time is later than OFF time.
        // e.g., ON 22:00, OFF 06:00
        return now_min_of_day >= on_min_of_day || now_min_of_day < off_min_of_day;
    }
}


esp_err_t schedule_reconcile(time_t last_seen_utc, time_t now_utc, const schedule_t *s, schedule_event_cb_t cb, void *arg)
{
    if (!s || !cb) return ESP_ERR_INVALID_ARG;
    if (last_seen_utc >= now_utc) return ESP_OK;

    // The most reliable way to reconcile is to check the state at `last_seen_utc`
    // and the state at `now_utc`. If they are different, we know a transition was missed.
    // A full reconciliation of all missed events is complex with DST. This is a robust simplification.

    bool was_on = is_currently_on(last_seen_utc, s);
    bool should_be_on = is_currently_on(now_utc, s);

    if (was_on != should_be_on) {
        ESP_LOGI(TAG, "Reconciling state change: was %s, should be %s", was_on ? "ON" : "OFF", should_be_on ? "ON" : "OFF");
        // We fire a single callback to get the system into the correct state.
        // We use `now_utc` as the event time.
        cb(should_be_on, now_utc, arg);
    } else {
        ESP_LOGI(TAG, "No state change to reconcile.");
    }

    return ESP_OK;
}

static void send_control_cmd(bool is_on)
{
    control_cmd_t cmd = {
        .actor = ACTOR_SCHEDULE,
        .ts = time(NULL),
        .light_pct = is_on ? 100 : 0,
        .pump_pct = is_on ? 100 : 0,
        .ramp_ms = 1000,
    };
    if (xQueueSend(g_cmd_queue, &cmd, 0) != pdPASS) {
        ESP_LOGW(TAG, "Failed to send command to control queue");
    }
}

static void schedule_task(void *arg)
{
    ESP_LOGI(TAG, "schedule_task starting");
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    // Wait for time to be synchronized before starting the main loop
    ESP_LOGI(TAG, "Waiting for time sync...");
    while (true) {
        EventBits_t b = xEventGroupWaitBits(g_net_state_event_group, NET_BIT_TIME_SYNCED,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(1000));
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        if (b & NET_BIT_TIME_SYNCED) break;
    }
    ESP_LOGI(TAG, "Time is synchronized");

    schedule_t s;
    schedule_load(&s);

    // Set initial state based on reconciliation
    bool initial_state = is_currently_on(time(NULL), &s);
    ESP_LOGI(TAG, "Initial schedule state is %s", initial_state ? "ON" : "OFF");
    send_control_cmd(initial_state);

    for (;;) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());

        // Reload schedule in case it was changed by another task (e.g., BLE)
        schedule_load(&s);

        time_t now_utc = time(NULL);
        time_t next_on_utc, next_off_utc;
        schedule_compute_next_events(now_utc, &s, &next_on_utc, &next_off_utc);

        time_t next_event_utc;
        bool is_next_event_on;

        if (next_on_utc < next_off_utc) {
            next_event_utc = next_on_utc;
            is_next_event_on = true;
        } else {
            next_event_utc = next_off_utc;
            is_next_event_on = false;
        }

        int32_t sleep_seconds = next_event_utc - now_utc;
        if (sleep_seconds <= 0) {
            sleep_seconds = 1; // Should not happen, but as a safeguard
        }

        ESP_LOGI(TAG, "Next event is %s in %d seconds (at %lld)",
                 is_next_event_on ? "ON" : "OFF", sleep_seconds, next_event_utc);

    // Sleep until the next event, but wake up periodically to reset WDT
    const int max_sleep_s = 10; // keep short to reliably feed WDT
        int remaining_sleep_s = sleep_seconds;
        while (remaining_sleep_s > 0) {
            int current_sleep_s = (remaining_sleep_s > max_sleep_s) ? max_sleep_s : remaining_sleep_s;
            vTaskDelay(pdMS_TO_TICKS(current_sleep_s * 1000));
            remaining_sleep_s -= current_sleep_s;
            ESP_ERROR_CHECK(esp_task_wdt_reset());
        }


        // Time for the event
        ESP_LOGI(TAG, "Event time: turning %s", is_next_event_on ? "ON" : "OFF");
        send_control_cmd(is_next_event_on);

        // Small delay to allow the command to be processed before we loop again
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
