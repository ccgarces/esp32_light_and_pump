#pragma once

#include <esp_err.h>

// AWS IoT Core MQTT (mTLS) helper. Provides Device Shadow and Jobs handling stubs.

esp_err_t aws_mqtt_init(void);

// Connect to AWS IoT using mTLS (device certs provided via partition or filesystem)
esp_err_t aws_mqtt_connect(void);

// Publish device shadow reported state (JSON string)
esp_err_t aws_publish_shadow(const char *reported_json);

// Request OTA via AWS Job ID (placeholder)
esp_err_t aws_handle_job(const char *job_id, const char *job_doc);

// Generic MQTT publish helper (requires aws_mqtt_connect first).
// qos: 0 or 1. Returns ESP_OK if queued.
esp_err_t aws_mqtt_publish(const char *topic, const char *data, int len, int qos);
