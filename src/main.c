/*
 * Main firmware for esp32_light_and_pump
 * Implements: WiFi (STA), MQTT client, I2C AHT10 reads, PWM control (LEDC), NVS storage,
 * hourly sensor logging, schedule-based light/pump control, OTA trigger via MQTT.
 *
 * NOTE: BLE commissioning is left as a TODO (see README). For now, WiFi/MQTT credentials
 * can be stored via NVS using the simple CLI helper in storage.c or via MQTT provisioning
 * topic when the device first connects.
 */
/*
 * Main firmware for esp32_light_and_pump
 */

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "esp_event.h"
#include "esp_sntp.h"

#include "wifi_manager.h"
#include "mqtt_manager.h"
#include "aht10.h"
#include "pwm_ctrl.h"
#include "storage.h"
#include "scheduler.h"
#include "ota_manager.h"
#include "http_server.h"
#include "ble_prov.h"

static const char *TAG = "main";

void time_sync_notification_cb(struct timeval *tv)
{
    ESP_LOGI(TAG, "Time synchronized");
}

static void sntp_init_and_wait(void)
{
    ESP_LOGI(TAG, "Initializing SNTP");
    esp_sntp_setoperatingmode(SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_set_time_sync_notification_cb(time_sync_notification_cb);
    esp_sntp_init();

    /* wait for time to be set */
    time_t now = 0;
    struct tm timeinfo = { 0 };
    int retry = 0;
    const int retry_count = 10;
    while (sntp_get_sync_status() == SNTP_SYNC_STATUS_RESET && ++retry < retry_count) {
        ESP_LOGI(TAG, "Waiting for system time to be set... (%d/%d)", retry, retry_count);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
    time(&now);
    localtime_r(&now, &timeinfo);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_event_loop_create_default());

    ESP_LOGI(TAG, "Initializing storage");
    storage_init();

    ESP_LOGI(TAG, "Initializing PWM controller");
    pwm_ctrl_init();

    ESP_LOGI(TAG, "Initializing I2C and AHT10 sensor");
    aht10_init();

    ESP_LOGI(TAG, "Starting WiFi manager");
    wifi_manager_init();

    /* Wait until WiFi is connected or timeout */
    if (wifi_manager_wait_connected(pdMS_TO_TICKS(15000))) {
        ESP_LOGI(TAG, "Connected to WiFi, init SNTP and MQTT");
        sntp_init_and_wait();
        mqtt_manager_init();
    /* start HTTP server so mobile app can fetch readings */
    http_server_init();
    } else {
        ESP_LOGW(TAG, "WiFi not connected - MQTT and SNTP will be delayed until connection");
    }

    ESP_LOGI(TAG, "Initializing scheduler (default schedule 07:00-21:00)");
    scheduler_init();

    ESP_LOGI(TAG, "Initializing OTA manager");
    ota_manager_init();

    /* Initialize BLE provisioning (stub) so README callers know it's available */
    ble_prov_init();

    /* Sensor task: read AHT10 hourly and store */
    xTaskCreatePinnedToCore(aht10_hourly_task, "aht10_task", 4 * 1024, NULL, 5, NULL, tskNO_AFFINITY);

    /* Scheduler task manages scheduled on/off for light and pump */
    xTaskCreatePinnedToCore(scheduler_task, "scheduler", 4 * 1024, NULL, 5, NULL, tskNO_AFFINITY);

    /* MQTT task runs inside mqtt_manager; it will publish sensor data when ready */

    ESP_LOGI(TAG, "Firmware initialization complete");
}
