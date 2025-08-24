#include "telemetry.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include "cJSON.h"
#include "ipc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "esp_wifi.h"
#include "esp_timer.h"
#include "schedule.h"
#include <time.h>
#include "sdkconfig.h"
#include "aws_mqtt.h"
#include "net.h"

static const char *TAG = "telemetry";

#define AUDIT_QUEUE_LEN 16
#define MAX_AUDIT_MSG_LEN 256

static QueueHandle_t s_audit_queue = NULL;

static esp_err_t publish_heartbeat(void) {
    if (!(xEventGroupGetBits(g_net_state_event_group) & NET_BIT_MQTT_UP)) {
        ESP_LOGD(TAG, "Skipping heartbeat, MQTT not connected");
    return ESP_ERR_INVALID_STATE;
    }

    cJSON *root = cJSON_CreateObject();
    if (!root) {
    ESP_LOGE(TAG, "Failed to create JSON object for heartbeat");
    return ESP_ERR_NO_MEM;
    }

    // Collect heartbeat info
    time_t now = time(NULL);
    uint64_t uptime = esp_timer_get_time() / 1000000ULL;
    esp_reset_reason_t rr = esp_reset_reason();
    size_t min_free = esp_get_minimum_free_heap_size();
    
    wifi_ap_record_t apinfo;
    int8_t rssi = (esp_wifi_sta_get_ap_info(&apinfo) == ESP_OK) ? apinfo.rssi : 127;

    schedule_t s;
    time_t next_on = 0, next_off = 0;
    if (schedule_load(&s) == ESP_OK) {
        schedule_compute_next_events(now, &s, &next_on, &next_off);
    }

    // Build JSON
    cJSON_AddNumberToObject(root, "ts", now);
    cJSON_AddNumberToObject(root, "uptime_s", uptime);
    cJSON_AddNumberToObject(root, "reset_reason", rr);
    cJSON_AddNumberToObject(root, "min_free_heap", min_free);
    cJSON_AddNumberToObject(root, "wifi_rssi", rssi);
    if (next_on > 0) cJSON_AddNumberToObject(root, "next_on_utc", next_on);
    if (next_off > 0) cJSON_AddNumberToObject(root, "next_off_utc", next_off);

    char *json_str = cJSON_PrintUnformatted(root);
    if (json_str) {
        ESP_LOGI(TAG, "Heartbeat: %s", json_str);
        aws_mqtt_publish(CONFIG_TELEMETRY_HEARTBEAT_TOPIC, json_str, (int)strlen(json_str), 1);
        free(json_str);
    }
    cJSON_Delete(root);
    return ESP_OK;
}

static void publish_audit_log(const char *msg) {
    if (!(xEventGroupGetBits(g_net_state_event_group) & NET_BIT_MQTT_UP)) {
        ESP_LOGD(TAG, "Skipping audit log, MQTT not connected");
        return;
    }
    ESP_LOGI(TAG, "Audit: %s", msg);
    aws_mqtt_publish(CONFIG_TELEMETRY_AUDIT_TOPIC, msg, (int)strlen(msg), 1);
}

static void telemetry_task(void *arg) {
    ESP_LOGI(TAG, "Telemetry task started");
    TickType_t last_heartbeat_tick = xTaskGetTickCount();

    for (;;) {
        // Wait for the next event: either a message in the audit queue or the heartbeat timer
        char audit_msg[MAX_AUDIT_MSG_LEN];
        TickType_t ticks_to_wait = pdMS_TO_TICKS(CONFIG_TELEMETRY_HEARTBEAT_INTERVAL_S * 1000);
        TickType_t elapsed_ticks = xTaskGetTickCount() - last_heartbeat_tick;
        
        if (elapsed_ticks >= ticks_to_wait) {
            // Time for a heartbeat
            publish_heartbeat();
            last_heartbeat_tick = xTaskGetTickCount();
            continue; // Re-evaluate wait time
        }

        ticks_to_wait -= elapsed_ticks;

        if (xQueueReceive(s_audit_queue, audit_msg, ticks_to_wait) == pdPASS) {
            publish_audit_log(audit_msg);
        } else {
            // Timeout occurred, time for a heartbeat
            publish_heartbeat();
            last_heartbeat_tick = xTaskGetTickCount();
        }
    }
}

esp_err_t telemetry_init(void) {
    s_audit_queue = xQueueCreate(AUDIT_QUEUE_LEN, MAX_AUDIT_MSG_LEN);
    if (!s_audit_queue) {
        ESP_LOGE(TAG, "Failed to create audit queue");
        return ESP_ERR_NO_MEM;
    }

    BaseType_t r = xTaskCreate(telemetry_task, "telemetry_task", 4096, NULL, 3, NULL);
    if (r != pdPASS) {
        vQueueDelete(s_audit_queue);
        s_audit_queue = NULL;
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t telemetry_audit_log(const char *format, ...) {
    if (!s_audit_queue) return ESP_ERR_INVALID_STATE;
    if (!format) return ESP_ERR_INVALID_ARG;

    char msg[MAX_AUDIT_MSG_LEN];
    va_list args;
    va_start(args, format);
    int len = vsnprintf(msg, sizeof(msg), format, args);
    va_end(args);

    if (len < 0) {
        return ESP_FAIL;
    }
    if (len >= sizeof(msg)) {
        ESP_LOGW(TAG, "Audit message truncated");
    }

    if (xQueueSend(s_audit_queue, msg, pdMS_TO_TICKS(10)) != pdPASS) {
        return ESP_ERR_TIMEOUT;
    }
    return ESP_OK;
}

esp_err_t telemetry_publish_heartbeat(void) {
    return publish_heartbeat();
}

