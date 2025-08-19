#pragma once

#include <stdint.h>

void mqtt_manager_init(void);
void mqtt_publish_sensor(int64_t timestamp, float temperature, float humidity);

// OTA hook referenced
void ota_manager_request_update(const char *url, const char *expected_sha256_hex);
