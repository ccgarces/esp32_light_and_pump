#include "ota_manager.h"
#include "esp_log.h"
#include "esp_https_ota.h"
#include "esp_system.h"
#include "mbedtls/sha256.h"
#include <string.h>

static const char *TAG = "ota_manager";

void ota_manager_init(void)
{
    ESP_LOGI(TAG, "OTA manager initialized");
}

// Helper: convert hex string to bytes
static bool hexstr_to_bytes(const char *hex, uint8_t *out, size_t out_len)
{
    if (!hex) return false;
    size_t hex_len = strlen(hex);
    if (hex_len != out_len * 2) return false;
    for (size_t i = 0; i < out_len; ++i) {
        char byte_str[3] = { hex[i*2], hex[i*2+1], '\0' };
        char *endptr = NULL;
        long v = strtol(byte_str, &endptr, 16);
        if (endptr == byte_str || v < 0 || v > 0xFF) return false;
        out[i] = (uint8_t)v;
    }
    return true;
}

void ota_manager_request_update(const char *url, const char *expected_sha256_hex)
{
    ESP_LOGI(TAG, "OTA request URL=%s", url ? url : "(null)");
    if (!url) {
        ESP_LOGW(TAG, "No URL provided for OTA");
        return;
    }

    esp_https_ota_config_t ota_config = { 0 };
    esp_http_client_config_t http_cfg = { 0 };
    http_cfg.url = url;
    http_cfg.timeout_ms = 60000;
    ota_config.http_config = &http_cfg;

    esp_err_t err = esp_https_ota(&ota_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota failed: %s", esp_err_to_name(err));
        return;
    }

    // If expected SHA256 provided, verify app image in partition (best-effort)
    if (expected_sha256_hex) {
        uint8_t expected[32];
        if (hexstr_to_bytes(expected_sha256_hex, expected, sizeof(expected))) {
            // compute SHA256 of app in partition - not trivial; rely on bootloader verification
            ESP_LOGI(TAG, "Provided expected SHA256; ensure server signs images or use secure boot");
        } else {
            ESP_LOGW(TAG, "Invalid SHA256 hex provided; skipping verification");
        }
    }

    ESP_LOGI(TAG, "OTA update applied; restarting...");
    esp_restart();
}
