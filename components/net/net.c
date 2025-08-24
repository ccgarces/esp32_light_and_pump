#include "net.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_task_wdt.h"
#include "nvs_flash.h"
#include <stdbool.h>
#include <sys/time.h>
#include "storage.h"
#include "ipc.h"
#include "sdkconfig.h"

static const char *TAG = "net";

#define STORAGE_KEY_WIFI "wifi_creds"
#define WIFI_MAX_RETRY   CONFIG_NET_WIFI_MAX_RETRY

typedef struct {
    char ssid[32];
    char psk[64];
} wifi_creds_t;

static wifi_creds_t s_creds;
static bool s_have_creds = false;
static int s_retry_count = 0;

// Forward declarations
static void net_task(void *arg);
static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

// Time sync: in IDF 5.5, prefer sntp via lwIP helper is optional; mark TIME_SYNCED when Wi-Fi comes up as a stub
static void mark_time_synced_stub(void) {
    xEventGroupSetBits(g_net_state_event_group, NET_BIT_TIME_SYNCED);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        xEventGroupClearBits(g_net_state_event_group, NET_BIT_WIFI_UP);
        if (s_retry_count < WIFI_MAX_RETRY) {
            esp_wifi_connect();
            s_retry_count++;
            ESP_LOGI(TAG, "Wi-Fi disconnected, retrying to connect... (attempt %d/%d)", s_retry_count, WIFI_MAX_RETRY);
        } else {
            ESP_LOGE(TAG, "Failed to connect to Wi-Fi after %d attempts.", WIFI_MAX_RETRY);
            // After max retries, we could trigger a fallback or alert
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t* event = (ip_event_got_ip_t*) event_data;
        ESP_LOGI(TAG, "Got IP:" IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_count = 0;
        xEventGroupSetBits(g_net_state_event_group, NET_BIT_WIFI_UP);
    mark_time_synced_stub();
    }
}

static esp_err_t load_credentials(void)
{
    size_t len = sizeof(s_creds);
    esp_err_t err = storage_load_config(STORAGE_KEY_WIFI, &s_creds, &len);
    if (err == ESP_OK && len == sizeof(s_creds)) {
        s_have_creds = (s_creds.ssid[0] != '\0');
        if (s_have_creds) {
            ESP_LOGI(TAG, "Loaded Wi-Fi credentials for SSID (redacted)");
        } else {
            ESP_LOGI(TAG, "No Wi-Fi credentials found in NVS.");
        }
    } else {
        ESP_LOGW(TAG, "Failed to load Wi-Fi credentials (err=%s), assuming none exist.", esp_err_to_name(err));
        s_have_creds = false;
    }
    return ESP_OK;
}

static void net_task(void *arg)
{
    ESP_LOGI(TAG, "net_task starting");
    ESP_ERROR_CHECK(esp_task_wdt_add(NULL));

    TickType_t wifi_up_timestamp = 0;
    TickType_t wifi_down_timestamp = 0;
    bool was_wifi_up = false;

    // Initially, BLE is active for provisioning if we don't have credentials
    if (!s_have_creds) {
        xEventGroupSetBits(g_net_state_event_group, NET_BIT_BLE_ACTIVE);
        ESP_LOGI(TAG, "No Wi-Fi credentials, BLE is active for provisioning.");
    }


    for (;;) {
        ESP_ERROR_CHECK(esp_task_wdt_reset());
        EventBits_t bits = xEventGroupGetBits(g_net_state_event_group);
        bool is_wifi_up = (bits & NET_BIT_WIFI_UP);

        if (is_wifi_up && !was_wifi_up) {
            // Wi-Fi just came up
            wifi_up_timestamp = xTaskGetTickCount();
            wifi_down_timestamp = 0;
            ESP_LOGI(TAG, "Wi-Fi connection established.");
        } else if (!is_wifi_up && was_wifi_up) {
            // Wi-Fi just went down
            wifi_down_timestamp = xTaskGetTickCount();
            wifi_up_timestamp = 0;
            ESP_LOGI(TAG, "Wi-Fi connection lost.");
        }

        // BLE Fallback Logic from Prompts.md
        // - BLE fallback if Wi-Fi unavailable >= 60s
        if (!is_wifi_up && wifi_down_timestamp > 0) {
            if ((xTaskGetTickCount() - wifi_down_timestamp) > pdMS_TO_TICKS(CONFIG_NET_BLE_FALLBACK_SEC * 1000)) {
                if (!(bits & NET_BIT_BLE_ACTIVE)) {
                    ESP_LOGI(TAG, "Wi-Fi down for >%ds, activating BLE fallback.", CONFIG_NET_BLE_FALLBACK_SEC);
                    xEventGroupSetBits(g_net_state_event_group, NET_BIT_BLE_ACTIVE);
                }
            }
        }

        // - Disable BLE if Wi-Fi stable >= 5m
        if (is_wifi_up && wifi_up_timestamp > 0) {
            if ((xTaskGetTickCount() - wifi_up_timestamp) > pdMS_TO_TICKS(CONFIG_NET_WIFI_STABLE_MIN * 60 * 1000)) {
                if (bits & NET_BIT_BLE_ACTIVE) {
                    ESP_LOGI(TAG, "Wi-Fi stable for >%dmin, deactivating BLE.", CONFIG_NET_WIFI_STABLE_MIN);
                    xEventGroupClearBits(g_net_state_event_group, NET_BIT_BLE_ACTIVE);
                }
            }
        }

        was_wifi_up = is_wifi_up;
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

esp_err_t net_init(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL, NULL));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));

    load_credentials();

    if (s_have_creds) {
    wifi_config_t wifi_config = { .sta = { {0} } };
    strncpy((char*)wifi_config.sta.ssid, s_creds.ssid, sizeof(wifi_config.sta.ssid)-1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid)-1] = '\0';
    strncpy((char*)wifi_config.sta.password, s_creds.psk, sizeof(wifi_config.sta.password)-1);
    wifi_config.sta.password[sizeof(wifi_config.sta.password)-1] = '\0';
        ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
        ESP_LOGI(TAG, "Starting Wi-Fi connection to %s...", wifi_config.sta.ssid);
        ESP_ERROR_CHECK(esp_wifi_start());
    } else {
        ESP_LOGI(TAG, "No credentials, Wi-Fi not started. Waiting for provisioning.");
    }

    BaseType_t r = xTaskCreate(net_task, "net_task", 4096, NULL, 5, NULL);
    if (r != pdPASS) {
        ESP_LOGE(TAG, "Failed to create net_task");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "net initialized");
    return ESP_OK;
}

esp_err_t net_set_credentials(const char *ssid, const char *psk)
{
    if (!ssid || strlen(ssid) == 0) return ESP_ERR_INVALID_ARG;

    wifi_creds_t creds = {0};
    strncpy(creds.ssid, ssid, sizeof(creds.ssid) - 1);
    if (psk) {
        strncpy(creds.psk, psk, sizeof(creds.psk) - 1);
    }

    esp_err_t err = storage_save_config(STORAGE_KEY_WIFI, &creds, sizeof(creds));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save Wi-Fi credentials: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Saved new Wi-Fi credentials for SSID: %s. Restarting connection.", creds.ssid);
    memcpy(&s_creds, &creds, sizeof(s_creds));
    s_have_creds = true;

    // Apply new credentials and reconnect
    wifi_config_t wifi_config = { .sta = { {0} } };
    strncpy((char*)wifi_config.sta.ssid, s_creds.ssid, sizeof(wifi_config.sta.ssid)-1);
    wifi_config.sta.ssid[sizeof(wifi_config.sta.ssid)-1] = '\0';
    strncpy((char*)wifi_config.sta.password, s_creds.psk, sizeof(wifi_config.sta.password)-1);
    wifi_config.sta.password[sizeof(wifi_config.sta.password)-1] = '\0';
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));

    s_retry_count = 0;
    esp_wifi_disconnect();
    esp_wifi_connect();

    return ESP_OK;
}
