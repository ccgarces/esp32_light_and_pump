#pragma once

#include <stdbool.h>

// Initialize BLE provisioning service (NimBLE if available). Write characteristic expects "SSID;PASSWORD".
void ble_prov_init(void);
