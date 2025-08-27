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
#include "control.h"

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

    BaseType_t r = xTaskCreate(schedule_task, "schedule_task", 4096, NULL, 5, NULL);
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
    // Preserve current pump setting; only change light according to schedule
    control_state_t st = {0};
    uint8_t pump_pct = 0;
    if (control_get_state(&st) == ESP_OK) {
        pump_pct = st.pump_pct;
    }
    control_cmd_t cmd = {
        .actor = ACTOR_SCHEDULE,
        .ts = time(NULL),
        .light_pct = is_on ? CONFIG_SCHEDULE_LIGHT_ON_PCT : 0,
        .pump_pct = pump_pct,
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
    // Print current UTC and local time after sync
    time_t now = time(NULL);
    struct tm tm_utc = {0}, tm_loc = {0};
    char buf_utc[32] = {0}, buf_loc[48] = {0};
    gmtime_r(&now, &tm_utc);
    localtime_r(&now, &tm_loc);
    strftime(buf_utc, sizeof(buf_utc), "%Y-%m-%d %H:%M:%S UTC", &tm_utc);
    strftime(buf_loc, sizeof(buf_loc), "%Y-%m-%d %H:%M:%S %Z", &tm_loc);
    ESP_LOGI(TAG, "Time is synchronized: %s | Local: %s | epoch=%lld",
             buf_utc, buf_loc, (long long)now);

    schedule_t s;
    schedule_load(&s);

    // Set initial state and remember it
    bool last_on = is_currently_on(time(NULL), &s);
    ESP_LOGI(TAG, "Initial schedule state is %s", last_on ? "ON" : "OFF");
    send_control_cmd(last_on);

    // Initialize pump cycle state
    uint32_t pump_on_pct = CONFIG_SCHEDULE_PUMP_ON_PCT;
    int pump_duration_min = CONFIG_SCHEDULE_PUMP_ON_DURATION_MIN;
    int pump_interval_min = CONFIG_SCHEDULE_PUMP_ON_INTERVAL_MIN;
    if (pump_interval_min < pump_duration_min) {
        ESP_LOGW(TAG, "Pump interval (%d) < duration (%d); clamping interval=duration", pump_interval_min, pump_duration_min);
        pump_interval_min = pump_duration_min;
    }
    time_t start_epoch = time(NULL); // anchor for cycles
    // Align to the next minute boundary for consistent cadence
    start_epoch = start_epoch - (start_epoch % 60);

    for (;;) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());

        // Reload schedule in case it was changed by another task (e.g., BLE)
        schedule_load(&s);

        time_t now_utc = time(NULL);
        bool should_be_on = is_currently_on(now_utc, &s);
        if (should_be_on != last_on) {
            ESP_LOGI(TAG, "Minute check: state changed -> %s", should_be_on ? "ON" : "OFF");
            send_control_cmd(should_be_on);
            last_on = should_be_on;
        } else {
            ESP_LOGD(TAG, "Minute check: no change (%s)", should_be_on ? "ON" : "OFF");
        }

        // Pump cycle: ON for duration then OFF, with interval
        // Compute minutes since anchor
        int minutes_since_anchor = (int)((now_utc - start_epoch) / 60);
        int minutes_into_cycle = minutes_since_anchor % pump_interval_min;
        bool pump_should_be_on = minutes_into_cycle < pump_duration_min;
        // Apply pump state without altering intended light schedule state
        uint8_t desired_light = last_on ? CONFIG_SCHEDULE_LIGHT_ON_PCT : 0;
        uint8_t desired_pump = pump_should_be_on ? pump_on_pct : 0;
        // Send only if a change is needed vs. last commanded state (approximate)
        static uint8_t last_cmd_light = 0, last_cmd_pump = 0;
        if (desired_light != last_cmd_light || desired_pump != last_cmd_pump) {
            control_cmd_t pump_cmd = {
                .actor = ACTOR_SCHEDULE,
                .ts = now_utc,
                .seq = 0,
                .light_pct = desired_light,
                .pump_pct = desired_pump,
                .ramp_ms = 500,
            };
            if (xQueueSend(g_cmd_queue, &pump_cmd, 0) != pdPASS) {
                ESP_LOGW(TAG, "Failed to send pump control command");
            } else {
                last_cmd_light = desired_light;
                last_cmd_pump = desired_pump;
                ESP_LOGI(TAG, "Pump %s (%u%%) [cycle %d/%d min]",
                         pump_should_be_on ? "ON" : "OFF", desired_pump,
                         minutes_into_cycle + 1, pump_interval_min);
            }
        }

        // Sleep until next minute boundary, feeding WDT in chunks
    int sec_into_min = (int)(now_utc % 60);
        int sleep_s = 60 - sec_into_min;
        if (sleep_s <= 0) sleep_s = 60;
    // Feed WDT more frequently than its timeout (typically 5s)
    const int chunk = 1;
        while (sleep_s > 0) {
            int d = (sleep_s > chunk) ? chunk : sleep_s;
            vTaskDelay(pdMS_TO_TICKS(d * 1000));
            sleep_s -= d;
            ESP_ERROR_CHECK(esp_task_wdt_reset());
        }
    }
}
