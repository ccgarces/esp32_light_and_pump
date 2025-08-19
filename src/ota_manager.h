#pragma once

// Initialize OTA manager
void ota_manager_init(void);

// Request OTA update from URL; expected_sha256_hex may be NULL to skip verification
void ota_manager_request_update(const char *url, const char *expected_sha256_hex);
