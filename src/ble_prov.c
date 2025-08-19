#include "ble_prov.h"
#include "esp_log.h"
#include "nvs.h"
#include "storage.h"
#include "wifi_manager.h"

static const char *TAG = "ble_prov";

void ble_prov_init(void)
{
    // Lightweight stub: full NimBLE GATT implementation is platform/version dependent.
    // Provide a simple log and rely on MQTT / CLI provisioning as fallback.
    ESP_LOGI(TAG, "BLE provisioning init (stub). For full BLE provisioning, implement NimBLE GATT here and write SSID;PASSWORD to storage and call wifi_manager_connect()");
}
