#pragma once

#include <stdint.h>
#include <stddef.h>
#include <esp_err.h>

// Storage component: NVS-backed storage with CRC + backup copy.
// Purpose: reliably store small configuration blobs (schedules, device config).
// Security/reliability: each blob stores a CRC; a backup copy is kept under "<key>_bak".
// On load, if the primary key is corrupt, the backup is used and restored.

/**
 * @brief Initialize storage subsystem (must be called before other APIs).
 *
 * This function initializes the NVS flash partition. It will erase and re-initialize
 * the partition if it's corrupted.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t storage_init(void);

/**
 * @brief Save a configuration blob to NVS.
 *
 * A CRC is calculated and appended to the data. A backup copy is also maintained
 * for reliability. The save order is backup then primary to ensure the backup is
 * always the last known good version.
 *
 * @param key The key to store the data under.
 * @param data Pointer to the data to save.
 * @param len Length of the data in bytes.
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t storage_save_config(const char *key, const void *data, size_t len);

/**
 * @brief Load a configuration blob from NVS.
 *
 * This function attempts to load data from the primary key. If the data is not found
 * or the CRC check fails, it will attempt to load from the backup key. If the backup
is valid, it will be used to restore the primary key.
 *
 * To get the required buffer size, call this function with `out_buf` as NULL. The
 * required size will be returned in `len`.
 *
 * @param key The key for the data to load.
 * @param out_buf Pointer to the buffer to receive the data. Can be NULL to query size.
 * @param len As input, the size of `out_buf`. As output, the actual size of the loaded data.
 * @return ESP_OK on success, ESP_ERR_NVS_NOT_FOUND if not found, or other error code.
 */
esp_err_t storage_load_config(const char *key, void *out_buf, size_t *len);

// Helpers used in tests and utility code
uint32_t storage_crc32(const void *data, size_t len);

// Build backup key name into out buffer as "<key>_bak"; returns out on success or NULL if too small
char *storage_make_backup_key(const char *key, char *out, size_t out_size);

// Simple uint32 convenience APIs
esp_err_t storage_save_uint32(const char *key, uint32_t value);
esp_err_t storage_load_uint32(const char *key, uint32_t *out_value);
