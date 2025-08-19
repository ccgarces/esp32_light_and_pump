#include "storage.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdint.h>

static const char *TAG = "storage";

void storage_init(void)
{
    // NVS already initialized in main, keep a log
    ESP_LOGI(TAG, "storage initialized (NVS)");
}

bool storage_get_wifi_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READONLY, &handle);
    if (err != ESP_OK) return false;

    size_t required = ssid_len;
    esp_err_t res = nvs_get_str(handle, "wifi_ssid", ssid, &required);
    if (res != ESP_OK) {
        nvs_close(handle);
        return false;
    }
    required = pass_len;
    res = nvs_get_str(handle, "wifi_pass", pass, &required);
    nvs_close(handle);
    return res == ESP_OK;
}

bool storage_set_wifi_credentials(const char *ssid, const char *pass)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open("storage", NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    esp_err_t res = nvs_set_str(handle, "wifi_ssid", ssid);
    if (res == ESP_OK) res = nvs_set_str(handle, "wifi_pass", pass);
    if (res == ESP_OK) res = nvs_commit(handle);
    nvs_close(handle);
    return res == ESP_OK;
}

bool storage_log_sensor_reading(int64_t timestamp, float temperature, float humidity)
{
    // For simplicity, append to NVS keys with incremental index; production should use wear-leveling storage
    nvs_handle_t handle;
    if (nvs_open("sensors", NVS_READWRITE, &handle) != ESP_OK) return false;
    uint32_t idx = 0;
    nvs_get_u32(handle, "idx", &idx);
    char key[32];
    snprintf(key, sizeof(key), "r%lu", (unsigned long)idx);
    char val[64];
    snprintf(val, sizeof(val), "%lld,%.2f,%.2f", timestamp, temperature, humidity);
    esp_err_t res = nvs_set_str(handle, key, val);
    if (res == ESP_OK) {
        idx++;
        nvs_set_u32(handle, "idx", idx);
        res = nvs_commit(handle);
    }
    nvs_close(handle);
    return res == ESP_OK;
}
