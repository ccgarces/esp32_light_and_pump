#pragma once

#include <stdint.h>
#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "net.h" // Reuse NET_BIT_* definitions

// This header defines the Inter-Process Communication (IPC) primitives
// used across the application, as specified in Prompts.md.

// --- Command Queue ---
// Used to send commands to the control_task.
typedef struct {
    uint32_t actor;     // Identifier for the command source (e.g., ble, schedule, net)
    uint64_t ts;        // Timestamp of command creation
    uint32_t seq;       // Sequence number
    uint8_t light_pct;  // Light intensity (0-100)
    uint8_t pump_pct;   // Pump pressure (0-100)
    uint32_t ramp_ms;   // Duration of the ramp in milliseconds
} control_cmd_t;

extern QueueHandle_t g_cmd_queue;


// --- Network State Event Group ---
// Used to signal the status of network connectivity.
extern EventGroupHandle_t g_net_state_event_group;

// Bits are defined in net.h

// Actor IDs (sources of control commands)
#define ACTOR_UNKNOWN    0
#define ACTOR_BLE        1
#define ACTOR_SCHEDULE   2
#define ACTOR_SAFETY     3


// --- Other Queues (to be implemented) ---
// extern QueueHandle_t g_schedule_queue;
// extern QueueHandle_t g_audit_queue;
// extern QueueHandle_t g_status_queue;

