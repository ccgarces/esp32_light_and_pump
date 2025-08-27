#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "safety.h"
#include "storage.h"
#include "control.h"
#include "schedule.h"
#include "net.h"
#include "ble.h"
#include "telemetry.h"
#include "ota.h"
#include "ipc.h"
#include "esp_ota_ops.h"
#include "aws_mqtt.h"
#include "esp_mac.h"

static const char *TAG = "app_main";

// Global IPC handles
QueueHandle_t g_cmd_queue = NULL;
EventGroupHandle_t g_net_state_event_group = NULL;

// Reconcile callback to apply schedule state
static void apply_schedule_cb(bool on, time_t ts, void *arg)
{
    // Preserve pump, only set light according to schedule default
    control_state_t st = {0};
    uint8_t pump_pct = 0;
    if (control_get_state(&st) == ESP_OK) pump_pct = st.pump_pct;
    control_cmd_t cmd = {
        .actor = ACTOR_SCHEDULE,
        .ts = ts,
        .seq = 0,
        .light_pct = on ? CONFIG_SCHEDULE_LIGHT_ON_PCT : 0,
        .pump_pct = pump_pct,
        .ramp_ms = 500,
    };
    xQueueSend(g_cmd_queue, &cmd, 0);
}

// BLE provisioning callback: receives ssid, psk, tz
static void on_ble_provisioned(const char *ssid, const char *psk, const char *tz, void *arg)
{
    ESP_LOGI(TAG, "BLE provisioning received (tz=%s)", tz ? tz : "");
    // store creds and trigger network
    if (ssid && ssid[0]) {
        net_set_credentials(ssid, psk);
        // Attempt to wipe PSK copy if provided
        if (psk) {
            volatile char *vp = (volatile char*)psk; // best-effort zeroize
            for (size_t i = 0; vp[i] && i < 64; ++i) vp[i] = 0;
        }
    }
    // update schedule tz
    schedule_t s;
    if (schedule_load(&s) == ESP_OK) {
        if (tz && tz[0]) {
            strncpy(s.tz, tz, sizeof(s.tz)-1);
            s.tz[sizeof(s.tz)-1] = '\0';
            schedule_save(&s);
            // apply TZ immediately for local schedule computations
            setenv("TZ", s.tz, 1);
            tzset();
            ESP_LOGI(TAG, "applied TZ=%s", s.tz);
        }
    }
}

void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);
    ESP_LOGI("APP", "=== USB console hello ===");
    ESP_LOGI(TAG, "starting app_main");

    // Print MAC early so onboarding tools/users can identify the device even if BLE is inactive
    uint8_t mac[6] = {0};
    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) == ESP_OK) {
        ESP_LOGI(TAG, "Device MAC (STA) %02X:%02X:%02X:%02X:%02X:%02X",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    }

    // If in pending-verify state after OTA, mark this app valid to cancel rollback.
    // Safe to call unconditionally; it no-ops if not pending.
    esp_ota_mark_app_valid_cancel_rollback();

    // Create global IPC primitives
    g_cmd_queue = xQueueCreate(8, sizeof(control_cmd_t));
    g_net_state_event_group = xEventGroupCreate();

    // Safety first: init safety/wdt
    safety_init();

    // Storage
    if (storage_init() != ESP_OK) ESP_LOGW(TAG, "storage_init failed");

    // Control (sets safe defaults OFF)
    if (control_init() != ESP_OK) ESP_LOGW(TAG, "control_init failed");

    // Schedule (loads defaults if none)
    if (schedule_init() != ESP_OK) ESP_LOGW(TAG, "schedule_init failed");
    schedule_t s;
    schedule_load(&s);
    // Apply stored timezone (if any) so schedule computations use local wall clock
    if (s.tz[0]) {
        setenv("TZ", s.tz, 1);
        tzset();
        ESP_LOGI(TAG, "applied stored TZ=%s", s.tz);
    }
    ESP_LOGI(TAG, "schedule: ON %02d:%02d OFF %02d:%02d TZ=%s", s.on_hour, s.on_min, s.off_hour, s.off_min, s.tz);

    // Register BLE provisioning callback to save credentials and tz
    ble_register_prov_callback(on_ble_provisioned, NULL);
    // BLE is started/stopped by the BLE manager based on network state to
    // minimize attack surface. The ble_init function is responsible for setting this up.
    if (ble_init() != ESP_OK) ESP_LOGW(TAG, "ble_init failed");

    // Net (wifi/sntp)
    if (net_init() != ESP_OK) ESP_LOGW(TAG, "net_init failed");

    // AWS MQTT: defer connect until Wi‑Fi is up and time is synced
    if (aws_mqtt_init() != ESP_OK) {
        ESP_LOGW(TAG, "aws_mqtt_init failed");
    }

    // Telemetry
    telemetry_init();

    // OTA
    ota_init();

    // Optionally reconcile missed schedule events since last boot; for demo we use boot time-60s as last seen
    // Wait for time to be synced before reconciling schedule
    ESP_LOGI(TAG, "Waiting for Wi‑Fi + time sync to start AWS MQTT...");
    EventBits_t bits = xEventGroupWaitBits(g_net_state_event_group, NET_BIT_WIFI_UP | NET_BIT_TIME_SYNCED, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if ((bits & (NET_BIT_WIFI_UP | NET_BIT_TIME_SYNCED)) == (NET_BIT_WIFI_UP | NET_BIT_TIME_SYNCED)) {
        if (aws_mqtt_connect() != ESP_OK) {
            ESP_LOGW(TAG, "aws_mqtt_connect failed");
        }
    } else {
        ESP_LOGW(TAG, "AWS start skipped (no Wi‑Fi/time). Will rely on later retries if implemented.");
    }
    ESP_LOGI(TAG, "Waiting for time sync...");
    bits = xEventGroupWaitBits(g_net_state_event_group, NET_BIT_TIME_SYNCED, pdFALSE, pdTRUE, pdMS_TO_TICKS(30000));
    if (bits & NET_BIT_TIME_SYNCED) {
        time_t now_utc = time(NULL);
        // A more robust implementation would store the last shutdown time in NVS
        time_t last_seen = now_utc - 60;
        ESP_LOGI(TAG, "Time synced. Reconciling schedule...");
    schedule_reconcile(last_seen, now_utc, &s, apply_schedule_cb, NULL);
    } else {
        ESP_LOGW(TAG, "Time not synced after 30s. Skipping schedule reconcile.");
    }


    ESP_LOGI(TAG, "init complete; application running");

    vTaskDelete(NULL);
}
