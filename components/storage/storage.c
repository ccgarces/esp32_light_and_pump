#include "storage.h"
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_crc.h"
#include "sdkconfig.h"

static const char *TAG = "storage";

static bool s_inited = false;

esp_err_t storage_init(void)
{
    if (s_inited) {
        return ESP_OK;
    }
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "NVS partition corrupted or full, erasing...");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_flash_init failed: %s", esp_err_to_name(err));
        return err;
    }
    s_inited = true;
    ESP_LOGI(TAG, "Storage initialized (namespace: %s)", CONFIG_STORAGE_NAMESPACE);
    return ESP_OK;
}

esp_err_t storage_save_config(const char *key, const void *data, size_t len)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!key || !data || len == 0) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    // Create a temporary buffer for data + CRC
    size_t blob_size = len + sizeof(uint32_t);
    uint8_t *blob = malloc(blob_size);
    if (!blob) {
        nvs_close(handle);
        return ESP_ERR_NO_MEM;
    }

    // Copy data and calculate CRC
    memcpy(blob, data, len);
    *(uint32_t*)(blob + len) = esp_crc32_le(0, data, len);

    // --- Save Strategy: Backup First ---
    // 1. Save to backup key
    char backup_key[NVS_KEY_NAME_MAX_SIZE];
    snprintf(backup_key, sizeof(backup_key), "%s_bak", key);
    err = nvs_set_blob(handle, backup_key, blob, blob_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save to backup key '%s': %s", backup_key, esp_err_to_name(err));
        goto cleanup;
    }

    // 2. Save to primary key
    err = nvs_set_blob(handle, key, blob, blob_size);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save to primary key '%s': %s", key, esp_err_to_name(err));
        goto cleanup;
    }

    // 3. Commit changes
    err = nvs_commit(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_commit failed: %s", esp_err_to_name(err));
    } else {
        ESP_LOGI(TAG, "Saved config for key '%s' (%d bytes)", key, len);
    }

cleanup:
    free(blob);
    nvs_close(handle);
    return err;
}

// Internal helper to load and verify a blob from a given key
static esp_err_t load_and_verify(nvs_handle_t handle, const char *key, void *out_buf, size_t *len)
{
    size_t required_len;
    esp_err_t err = nvs_get_blob(handle, key, NULL, &required_len);
    if (err != ESP_OK) {
        return err;
    }

    if (required_len <= sizeof(uint32_t)) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    size_t data_len = required_len - sizeof(uint32_t);

    // If out_buf is NULL, we're just querying the size.
    if (out_buf == NULL) {
        *len = data_len;
        return ESP_OK;
    }

    // If the provided buffer is too small.
    if (*len < data_len) {
        *len = data_len;
        return ESP_ERR_NVS_INVALID_LENGTH;
    }

    uint8_t *blob = malloc(required_len);
    if (!blob) {
        return ESP_ERR_NO_MEM;
    }

    err = nvs_get_blob(handle, key, blob, &required_len);
    if (err != ESP_OK) {
        free(blob);
        return err;
    }

    uint32_t stored_crc = *(uint32_t*)(blob + data_len);
    uint32_t computed_crc = esp_crc32_le(0, blob, data_len);

    if (stored_crc != computed_crc) {
        ESP_LOGW(TAG, "CRC mismatch for key '%s'. Stored: 0x%x, Computed: 0x%x", key, stored_crc, computed_crc);
        free(blob);
        return ESP_ERR_INVALID_CRC;
    }

    memcpy(out_buf, blob, data_len);
    *len = data_len;

    free(blob);
    return ESP_OK;
}

esp_err_t storage_load_config(const char *key, void *out_buf, size_t *len)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!key || !len) return ESP_ERR_INVALID_ARG;

    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open failed: %s", esp_err_to_name(err));
        return err;
    }

    // 1. Try loading from the primary key
    err = load_and_verify(handle, key, out_buf, len);
    if (err == ESP_OK) {
        nvs_close(handle);
        ESP_LOGD(TAG, "Loaded config for key '%s'", key);
        return ESP_OK;
    }

    // 2. If primary fails (not found or corrupt), try the backup
    ESP_LOGW(TAG, "Primary key '%s' failed (%s). Trying backup.", key, esp_err_to_name(err));
    char backup_key[NVS_KEY_NAME_MAX_SIZE];
    snprintf(backup_key, sizeof(backup_key), "%s_bak", key);

    size_t backup_len = *len; // Preserve original buffer length for the call
    esp_err_t backup_err = load_and_verify(handle, backup_key, out_buf, &backup_len);

    if (backup_err == ESP_OK) {
        ESP_LOGI(TAG, "Loaded config from backup key '%s'. Restoring primary key.", backup_key);
        *len = backup_len; // Update the output length

        // If we had a buffer, restore the primary key now
        if (out_buf != NULL) {
            size_t blob_size = backup_len + sizeof(uint32_t);
            uint8_t *blob = malloc(blob_size);
            if (blob) {
                memcpy(blob, out_buf, backup_len);
                *(uint32_t*)(blob + backup_len) = esp_crc32_le(0, out_buf, backup_len);
                nvs_set_blob(handle, key, blob, blob_size);
                nvs_commit(handle);
                free(blob);
            }
        }
    } else {
        ESP_LOGE(TAG, "Backup key '%s' also failed (%s).", backup_key, esp_err_to_name(backup_err));
        err = backup_err; // Return the backup error
    }

    nvs_close(handle);
    return err;
}

uint32_t storage_crc32(const void *data, size_t len)
{
    return esp_crc32_le(0, data, len);
}

char *storage_make_backup_key(const char *key, char *out, size_t out_size)
{
    if (!key || !out || out_size == 0) return NULL;
    size_t need = strlen(key) + 4 + 1; // _bak + NUL
    if (need > out_size) return NULL;
    strcpy(out, key);
    strcat(out, "_bak");
    return out;
}

esp_err_t storage_save_uint32(const char *key, uint32_t value)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!key) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_set_u32(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return err;
}

esp_err_t storage_load_uint32(const char *key, uint32_t *out_value)
{
    if (!s_inited) return ESP_ERR_INVALID_STATE;
    if (!key || !out_value) return ESP_ERR_INVALID_ARG;
    nvs_handle_t handle;
    esp_err_t err = nvs_open(CONFIG_STORAGE_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return err;
    err = nvs_get_u32(handle, key, out_value);
    nvs_close(handle);
    return err;
}
