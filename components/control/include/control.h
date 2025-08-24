#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <esp_err.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"

// Public API for PWM control of grow light and air pump.
// Security/safety: outputs default to OFF on init. All public functions are thread-safe.

typedef struct {
    uint8_t light_pct; // 0-100
    uint8_t pump_pct;  // 0-100
} control_state_t;

// Initialize control component. Creates control task, LEDC, and watchdog registration.
// The control task listens on the global g_cmd_queue for commands.
esp_err_t control_init(void);

// Get last applied state (thread-safe snapshot)
esp_err_t control_get_state(control_state_t *out_state);

// Visible helper for unit tests: compute step count for a given ramp and step_ms.
uint32_t control_calc_step_count(uint32_t ramp_ms, uint32_t step_ms);
