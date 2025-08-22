# ESP32 Grow Light & Air Pump Controller — Requirements Specification
## 1. Overview

This firmware controls a grow light (intensity 0–100%) and an air pump (pressure 0–100%) via two independent PWM channels on an ESP32 device.
It is built with ESP-IDF (no Arduino, no super loop), with security and reliability as top priorities.

The device must operate autonomously with default schedules, support local provisioning and control via BLE, and optionally integrate with AWS IoT Core for cloud-based management and telemetry.

## 2. Functional Requirements
### 2.1 Device Control

Two independent PWM channels:

CH0 – Grow Light (high frequency, flicker-free)

CH1 – Air Pump (frequency tuned for motor/driver)

PWM duty cycle 0–100% adjustable by user.

Soft-ramp transitions to reduce inrush and extend component life.

### 2.2 Scheduling

Default behavior:

Lights ON at 07:00, OFF at 21:00 local time

Pump follows the same schedule (configurable later)

Schedules can be:

Locally set via Flutter app (BLE → Wi-Fi)

Remotely managed via AWS IoT Device Shadow

Time handling:

Device syncs with SNTP on Wi-Fi connect

Stores schedules in UTC and applies them with the configured timezone (IANA TZ string)

Handles DST and reconciles missed events after reboot

### 2.3 Connectivity

BLE onboarding:

Flutter app provides Wi-Fi credentials, location, timezone via QR code or pairing code

BLE session secured with proof-of-possession (PoP) + ephemeral key exchange

Wi-Fi primary channel:

mTLS to AWS IoT Core

MQTT topics for desired/reported state & telemetry

BLE fallback channel:

Activates if Wi-Fi is unavailable for ≥ 60s

Deactivates after Wi-Fi is stable for ≥ 5 minutes

BLE has priority if both are available (local operator override)

Same JSON schema used over BLE and MQTT

### 2.4 OTA Updates

OTA performed over Wi-Fi via AWS IoT Jobs

Features:

Signed firmware images (ECDSA)

Dual-bank OTA with rollback if new image fails to boot

Anti-rollback using secure version field

Staged rollout (1% → 5% → 25% → 100%)

## 3. Security Requirements

Secure Boot v2 enabled and fused at manufacturing

Flash Encryption enabled and fused

Per-device X.509 certificate stored in secure partition (esp_secure_cert)

All cloud connections use mTLS (TLS1.2/1.3)

BLE sessions:

Established with PoP

X25519 ECDH for ephemeral session key

All commands MAC-protected with nonce to prevent replay

Sessions expire after inactivity

Least-privilege IAM policies in AWS (topic-scoped to device ID)

Audit logging of all commands (actor: app_ble, app_cloud, scheduler, timestamp, values)

## 4. Reliability Requirements

Default safe state: light & pump OFF on undefined state or after crash

Brown-out detector enabled

System + task watchdogs enabled

Crash dumps stored to flash; uploaded on next cloud reconnect

Config integrity: CRC + backup copy in NVS

Heartbeat telemetry: uptime, reset reason, heap low-water mark, Wi-Fi RSSI

## 5. Telemetry & Logging

Periodic status (every N minutes):

Uptime

Heap low-water mark

Wi-Fi RSSI

Boot reason / reset cause

Next scheduled event

Event logs:

State changes (actor, timestamp, values)

OTA updates (version, status, rollback if any)

Security events (invalid PoP, replay attempt, OTA signature failure)

## 6. Partitioning

Recommended partition layout:

| Name            | Type | SubType |   Offset |     Size |
|-----------------|:----:|:-------:|---------:|---------:|
| nvs             | data | nvs     |  0x9000  |  0x6000  |
| nvs_keys        | data | key     |  0xF000  |  0x3000  |
| phy_init        | data | phy     | 0x12000  |  0x1000  |
| factory         | app  | factory | 0x20000  |     1M   |
| ota_0           | app  | ota_0   | 0x120000 |     1M   |
| ota_1           | app  | ota_1   | 0x220000 |     1M   |
| esp_secure_cert | data | 0x83    | 0x320000 |  0x6000  |
| storage         | data | fat     | 0x330000 | 0x200000 |


## 7. Development Environment

ESP-IDF (stable LTS release) with official ESP-IDF Tools for Windows

VS Code ESP-IDF extension for build, flash, monitor, debug

Unity test framework for unit tests

Flutter app for BLE commissioning and control

AWS IoT Core for optional cloud control, telemetry, OTA

## 8. Testing Requirements
Unit tests

Schedule conversions (local TZ ↔ UTC, DST handling)

Missed-event reconciliation after reboot

PWM ramp profiles and clamping

BLE session establishment, nonce replay prevention

OTA signature validation + rollback path

NVS corruption recovery

Field tests

Wi-Fi dropout → BLE fallback → Wi-Fi recovery

OTA staged rollout with forced boot failure → rollback confirmed

Brown-out / reset stress test → safe state + log

BLE vs cloud conflict → BLE overrides with proper audit trail

## 9. Non-Functional Requirements

Security-first design: no default credentials, all comms encrypted

Resilience: operates safely and predictably under resets, power loss, network dropouts

Observability: telemetry & logs sufficient to diagnose field issues remotely

Scalability: must support fleet management via AWS IoT (device shadow, OTA, telemetry aggregation)

## 10. FreeRTOS Architecture

| Task             | Purpose                                                                    | Priority\* | Stack (bytes) | IPC In                             | IPC Out                                                                         |
| ---------------- | -------------------------------------------------------------------------- | ---------: | ------------: | ---------------------------------- | ------------------------------------------------------------------------------- |
| `net_task`       | Wi-Fi lifecycle, SNTP, AWS IoT connect/reconnect, BLE fallback arbitration |      **5** |         8–12k | `net_evt_queue`                    | `net_state_pub` (event group bits), `cmd_queue` (cloud cmds), `telemetry_queue` |
| `ble_task`       | Secure provisioning + encrypted local control over GATT                    |      **5** |        10–14k | `ble_evt_queue`                    | `cmd_queue`, `audit_queue`, `net_state_req`                                     |
| `control_task`   | Apply desired state, PWM ramps, safety interlocks                          |      **6** |          6–8k | `cmd_queue`, `schedule_queue`      | `status_queue`, `audit_queue`                                                   |
| `schedule_task`  | Compute next on/off from UTC+TZ, catch-up on missed events                 |      **4** |          6–8k | `time_sync_evt`, `sched_cfg_queue` | `schedule_queue`, `audit_queue`                                                 |
| `ota_task`       | AWS IoT Jobs client, image download/verify/swap, rollback                  |      **4** |        10–16k | `ota_job_queue`                    | `ota_status_queue`, `audit_queue`                                               |
| `telemetry_task` | Heartbeats, metrics, crash dump upload                                     |      **3** |          6–8k | `status_queue`, `net_state_pub`    | MQTT publishes (via `net_task`), logs                                           |
| `storage_task`   | NVS read/write, config CRC/backup, atomic commits                          |      **3** |          6–8k | `cfg_write_queue`                  | `cfg_result_queue`                                                              |
| `safety_task`    | Brown-out notifications, WDT kicks, thermal/current alarms                 |      **7** |          4–6k | HW ISRs (deferred)                 | `cmd_queue` (force safe), `audit_queue`                                         |
