#pragma once

#include <esp_err.h>

// BLE provisioning and secure control component.
// Exposes a GATT service for provisioning credentials and establishing a secure AEAD session.
// A manager task handles starting/stopping BLE based on network state.

/**
 * @brief Initialize the BLE component.
 *
 * This function initializes the BLE controller and creates the BLE manager task.
 * The task is responsible for starting/stopping services based on network state.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t ble_init(void);

/**
 * @brief Stop the BLE component.
 *
 * This function stops the BLE manager task and de-initializes the BLE stack.
 *
 * @return ESP_OK on success.
 */
esp_err_t ble_stop(void);


/**
 * @brief Provisioning callback function type.
 * @param ssid The Wi-Fi SSID.
 * @param psk The Wi-Fi PSK.
 * @param tz The IANA timezone string.
 * @param arg User-provided argument.
 */
typedef void (*ble_prov_cb_t)(const char *ssid, const char *psk, const char *tz, void *arg);

/**
 * @brief Register a callback to be invoked when provisioning is complete.
 *
 * @param cb The callback function.
 * @param arg An optional argument to pass to the callback.
 */
void ble_register_prov_callback(ble_prov_cb_t cb, void *arg);
