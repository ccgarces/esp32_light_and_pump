#pragma once

#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Event bits published by net component
#define NET_BIT_WIFI_UP      (1<<0)
#define NET_BIT_MQTT_UP      (1<<1)
#define NET_BIT_TIME_SYNCED  (1<<2)
#define NET_BIT_BLE_ACTIVE   (1<<3)

// Initialize networking subsystem (starts Wi-Fi, SNTP, and management task)
esp_err_t net_init(void);

// Set Wi-Fi credentials, save them to NVS, and trigger a connection attempt.
esp_err_t net_set_credentials(const char *ssid, const char *psk);
