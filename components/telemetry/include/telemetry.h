#pragma once

#include <esp_err.h>

/**
 * @brief Public API for the telemetry component.
 *
 * This component provides a centralized way to handle telemetry data,
 * including periodic heartbeats and event-driven audit logs. It operates
 * in a background task and handles publishing over MQTT when the network
 * is available.
 */

/**
 * @brief Initialize the telemetry component.
 *
 * This function creates the telemetry task and the audit log queue.
 *
 * @return ESP_OK on success, or an error code on failure.
 */
esp_err_t telemetry_init(void);

/**
 * @brief Enqueue an audit log entry for publishing.
 *
 * This function is non-blocking and safe to call from any task.
 * The message is copied and queued for the telemetry task to publish.
 *
 * @param format The format string for the log message (printf-style).
 * @param ... Variable arguments for the format string.
 * @return ESP_OK if the message was queued successfully, ESP_ERR_NO_MEM if
 *         memory allocation failed, or ESP_ERR_TIMEOUT if the queue was full.
 */
esp_err_t telemetry_audit_log(const char *format, ...);

// Test/helper: trigger an immediate heartbeat publish once.
esp_err_t telemetry_publish_heartbeat(void);

