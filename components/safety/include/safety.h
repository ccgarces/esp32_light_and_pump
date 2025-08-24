#pragma once

#include <esp_err.h>

// Safety: brown-out handling, task/system watchdog registration, and overcurrent/thermal stubs.

/**
 * @brief Initialize the safety component.
 *
 * This function initializes the task watchdog timer and creates the safety_task.
 * The safety_task is a high-priority task responsible for monitoring system
 * health and responding to critical events.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t safety_init(void);

/**
 * @brief Trigger an immediate, safe shutdown of all actuators.
 *
 * This function sends a command to the control task to set all outputs to 0
 * with no ramp time. It can be called from any task.
 *
 * @return ESP_OK if the command was sent successfully, or an error code.
 */
esp_err_t safety_safe_shutdown(void);
