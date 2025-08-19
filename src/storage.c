#include "storage.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"
#include <string.h>
#include <stdint.h>
#include <stdlib.h>
#include <stdio.h>

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

int storage_export_readings_json(char *buf, size_t buflen)
{
    nvs_handle_t handle;
    if (nvs_open("sensors", NVS_READONLY, &handle) != ESP_OK) return -1;
    uint32_t idx = 0;
    nvs_get_u32(handle, "idx", &idx);

    size_t pos = 0;
    int written = snprintf(buf + pos, buflen - pos, "[");
    if (written < 0) { nvs_close(handle); return -1; }
    pos += written;

    for (uint32_t i = 0; i < idx; ++i) {
        char key[32];
        snprintf(key, sizeof(key), "r%lu", (unsigned long)i);
        size_t needed = 0;
        if (nvs_get_str(handle, key, NULL, &needed) != ESP_OK) continue;
        char *val = malloc(needed);
        if (!val) continue;
        if (nvs_get_str(handle, key, val, &needed) == ESP_OK) {
            // val is "timestamp,temp,hum"
            // We'll output as JSON object
            long long ts = 0; float t=0,h=0;
            sscanf(val, "%lld,%f,%f", &ts, &t, &h);
            // add comma separator for subsequent items
            if (i > 0) {
                if (pos + 1 < buflen) { buf[pos++] = ','; } else { free(val); break; }
            }
            written = snprintf(buf + pos, buflen - pos, "{\"ts\":%lld,\"t\":%.2f,\"h\":%.2f}", ts, t, h);
            if (written < 0 || (size_t)written >= buflen - pos) { free(val); break; }
            pos += written;
        }
        free(val);
    }

    if (pos + 2 < buflen) {
        buf[pos++] = ']';
        buf[pos] = '\0';
    } else if (buflen > 0) {
        buf[buflen-1] = '\0';
    }

    nvs_close(handle);
    return (int)pos;
}
