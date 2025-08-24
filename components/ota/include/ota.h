#pragma once

#include <esp_err.h>
#include <stdbool.h>
#include <stdbool.h>

/**
 * @brief Public API for the Over-the-Air (OTA) update component.
 *
 * This component handles firmware updates in a dedicated background task.
 * Updates are triggered by passing a JSON manifest.
 */

/**
 * @brief Initialize the OTA component.
 *
 * This function creates the OTA task, which is responsible for handling
 * firmware update jobs received via a queue.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t ota_init(void);

/**
 * @brief Trigger an OTA update by sending a manifest to the OTA task.
 *
 * This function is non-blocking. It copies the manifest string and queues it
 * for processing by the background OTA task.
 *
 * @param manifest_json A string containing the JSON manifest for the update.
 * @return ESP_OK if the job was queued successfully, or an error code on failure.
 */
esp_err_t ota_trigger_update(const char *manifest_json);

// Lightweight helpers used by unit tests; production versions can be internal.
typedef struct {
	uint32_t version;
	bool has_min_required;
	uint32_t min_required;
} ota_manifest_t;

// Parse minimal manifest fields for tests
esp_err_t ota_parse_manifest(const char *json, ota_manifest_t *out);

// Compute a hex key-id and short id from DER cert; returns non-zero on error
int ota_compute_keyid_from_der(const unsigned char *der, size_t der_len,
							   char *out_full_hex, size_t full_sz,
							   char *out_short_hex, size_t short_sz,
							   size_t short_nibbles);

// Version acceptance policy used in tests
bool ota_check_version_policy(uint32_t current, uint32_t newv, uint32_t min_required, bool allow_equal, bool allow_rollback);
