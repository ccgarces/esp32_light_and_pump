#pragma once

#include <stdbool.h>
#include "freertos/FreeRTOS.h"

void wifi_manager_init(void);
bool wifi_manager_wait_connected(TickType_t ticks);
// Connect immediately using given credentials (used after BLE/MQTT provisioning)
void wifi_manager_connect(const char *ssid, const char *pass);
