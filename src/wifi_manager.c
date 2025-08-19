#include "wifi_manager.h"
#include "esp_log.h"
#include "esp_event.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "storage.h"

static const char *TAG = "wifi_manager";
static EventGroupHandle_t s_wifi_event_group;
const int WIFI_CONNECTED_BIT = (1 << 0);

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);
static void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data);

void wifi_init_sta(const char *ssid, const char *pass)
{
    esp_netif_init();
    s_wifi_event_group = xEventGroupCreate();
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL, NULL));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &ip_event_handler, NULL, NULL));

    wifi_config_t wifi_config = { 0 };
    strncpy((char *)wifi_config.sta.ssid, ssid, sizeof(wifi_config.sta.ssid) - 1);
    strncpy((char *)wifi_config.sta.password, pass, sizeof(wifi_config.sta.password) - 1);

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(ESP_IF_WIFI_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "wifi_init_sta finished.");
}

bool wifi_manager_wait_connected(TickType_t ticks)
{
    if (s_wifi_event_group == NULL) return false;
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, ticks);
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void wifi_manager_init(void)
{
    /* Load credentials from storage; if not present, device remains offline until provisioned. */
    char ssid[64] = {0};
    char pass[64] = {0};
    if (storage_get_wifi_credentials(ssid, sizeof(ssid), pass, sizeof(pass))) {
        ESP_LOGI(TAG, "Found stored WiFi credentials, connecting to %s", ssid);
        wifi_init_sta(ssid, pass);
    } else {
        ESP_LOGW(TAG, "No WiFi credentials stored. Device remains offline until provisioned.");
        /* TODO: implement BLE provisioning - not implemented here. */
    }
}

void wifi_manager_connect(const char *ssid, const char *pass)
{
    ESP_LOGI(TAG, "wifi_manager_connect called for SSID=%s", ssid);
    wifi_init_sta(ssid, pass);
}

static void wifi_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "disconnected. Reconnect...");
        esp_wifi_connect();
        if (s_wifi_event_group) xEventGroupClearBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

static void ip_event_handler(void* arg, esp_event_base_t event_base, int32_t event_id, void* event_data)
{
    if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (s_wifi_event_group) xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}
