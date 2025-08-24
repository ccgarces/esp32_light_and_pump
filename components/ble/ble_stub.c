#include "ble.h"
#include "esp_err.h"
#include "esp_log.h"

static const char *TAG = "ble_stub";

esp_err_t ble_init(void)
{
    ESP_LOGW(TAG, "BLE stub active on this target; real BLE disabled");
    return ESP_OK;
}

esp_err_t ble_stop(void)
{
    return ESP_OK;
}

void ble_register_prov_callback(ble_prov_cb_t cb, void *arg)
{
    (void)cb; (void)arg;
}
