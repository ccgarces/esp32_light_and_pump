#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

void storage_init(void);

bool storage_get_wifi_credentials(char *ssid, size_t ssid_len, char *pass, size_t pass_len);
bool storage_set_wifi_credentials(const char *ssid, const char *pass);

bool storage_log_sensor_reading(int64_t timestamp, float temperature, float humidity);

// Export stored readings into a JSON buffer. `buf` must be provided with size buflen.
// Returns number of bytes written (excluding null) or -1 on error.
int storage_export_readings_json(char *buf, size_t buflen);
