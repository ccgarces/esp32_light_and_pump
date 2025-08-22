# Copilot Vibe Coding Prompt for ESP32 Grow Light & Air Pump Controller

You are coding an ESP32 firmware project using **ESP-IDF** and FreeRTOS.  
Do not use Arduino or super-loop code.  
Use **tasks, queues, event groups, and watchdogs**.  
The project is developed with the **ESP-IDF VS Code extension** on Windows.  
Security and reliability are top priorities.

## Project Requirements

1. **Two independent PWM channels**
   - CH0: Grow Light intensity control (0–100%)
   - CH1: Air Pump pressure control (0–100%)
   - Implement soft-ramp transitions.

2. **Secure Boot & Flash Encryption**
   - Enable Secure Boot v2 and Flash Encryption.
   - Use per-device X.509 certificate in `esp_secure_cert` partition.

3. **BLE commissioning**
   - Provision Wi-Fi SSID/PSK, timezone, location via Flutter app.
   - Use QR/PoP + ECDH session key, AEAD for secure BLE control.
   - BLE fallback if Wi-Fi unavailable ≥60s; disable if Wi-Fi stable ≥5m.
   - BLE has control priority if both channels active.

4. **Wi-Fi + AWS IoT Core (optional)**
   - mTLS MQTT connection using device certs.
   - Device Shadow to set/retrieve schedules and states.
   - Publish telemetry and audit logs.
   - OTA firmware updates via AWS IoT Jobs (signed, staged rollout, rollback on failure).

5. **Time & scheduling**
   - Sync SNTP on Wi-Fi connect.
   - Store schedules in UTC + TZ from app (IANA ID).
   - Default: ON 07:00, OFF 21:00.
   - Reconcile missed events after reboot.

6. **Reliability**
   - Brown-out detector enabled.
   - System and task watchdogs.
   - Safe default state = light and pump OFF.
   - Crash dumps stored and uploaded on reconnect.
   - Config CRC + backup copy in NVS.

7. **Telemetry**
   - Periodic heartbeat: uptime, reset reason, heap low-water, Wi-Fi RSSI, next schedule event.
   - Event logs for state changes, OTA status, security failures.

8. **Partition layout**
   - factory, ota_0, ota_1, nvs, nvs_keys, esp_secure_cert, phy_init, storage (logs).

9. **Testing**
   - Use Unity test framework for unit tests (scheduler, control logic, crypto, storage, OTA).
   - Manual tests: Wi-Fi flap, BLE fallback, OTA rollback, brown-out recovery.

## FreeRTOS Task Architecture

- **net_task**: Wi-Fi, SNTP, AWS IoT MQTT, BLE arbitration (priority 5)
- **ble_task**: Provisioning + secure GATT control (priority 5)
- **control_task**: Apply commands to PWM channels, ramp outputs, safety interlocks (priority 6)
- **schedule_task**: Compute on/off events from UTC+TZ, handle DST and catch-up (priority 4)
- **ota_task**: Manage OTA jobs, download, verify signatures, rollback (priority 4)
- **telemetry_task**: Publish heartbeat, metrics, crash dumps (priority 3)
- **storage_task**: NVS read/write with CRC + backup (priority 3)
- **safety_task**: Watchdogs, brown-out, overcurrent/thermal alarms (priority 7)

IPC:
- `cmd_queue`: incoming commands `{actor, ts, seq, {light%, pump%, ramp_ms}}`
- `schedule_queue`: scheduler events to control
- `audit_queue`: log entries
- `status_queue`: metrics and state
- `net_state_pub`: event group for link status bits (WIFI_UP, MQTT_UP, TIME_SYNCED, BLE_ACTIVE)

---

## Copilot Coding Style Guidelines

- Always generate **ESP-IDF C code** (`.c` + `.h` files), not Arduino.
- Organize code into **components/** (e.g., `control`, `ble`, `net`, `ota`, `storage`, `telemetry`, `safety`, `schedule`).
- Use `esp_log.h` for logging (`ESP_LOGI/W/E`).
- Use **Kconfig** options for pins, default schedule, MQTT endpoint, etc.
- Use **Unity test framework** (`components/<name>/test/`) for unit tests.
- For OTA: use IDF OTA APIs (`esp_https_ota`), enforce ECDSA signature, and rollback.
- For BLE: create custom GATT service; use secure session with nonce + AEAD.
- For PWM: use LEDC driver with mutex for safe updates.

---

## Task

Generate production-grade ESP-IDF code for this project, one component at a time.  
Each function must follow the above requirements.  
Include comments explaining purpose and security considerations.  
Generate tests for each component in Unity.
