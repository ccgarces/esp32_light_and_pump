#include "ota_manager.h"
#include "esp_log.h"

static const char *TAG = "ota_manager";

void ota_manager_init(void)
{
    ESP_LOGI(TAG, "OTA manager initialized (manual trigger via MQTT)");
}

void ota_manager_request_update(void)
{
    ESP_LOGI(TAG, "OTA update requested (no-op stub). Implement platform OTA logic here.");
}
