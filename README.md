# ESP32 Grow Light & Pump Controller

This firmware implements control for a grow light (PWM) and an air pump (PWM), reads temperature and humidity from an AHT10 sensor over I2C, stores hourly sensor readings in NVS, and exposes a basic TCP control interface for remote control.

Features

- PWM control for grow light and pump (LEDC)
- I2C driver for AHT10 temperature/humidity sensor
- Hourly sensor measurements stored in NVS (ring buffer)
- Wi‑Fi connection using credentials stored in NVS
- SNTP time sync when connected (timezone read from NVS, default UTC)
- Simple scheduler: default ON at 07:00, OFF at 21:00 (configurable via NVS keys)
- Simple TCP control server (port 3333) supporting commands like `SET LIGHT <duty>` and `SET PUMP <duty>`

Notes / TODO

- BLE provisioning (commissioning via Flutter app) is not implemented in this initial commit. The firmware reads Wi‑Fi credentials from NVS keys `wifi_ssid` and `wifi_pass`. Implement a BLE GATT service in the future to accept credentials from the mobile app and store them in NVS.
- OTA update initialization is left as a placeholder. Integrate ESP-IDF OTA (esp_https_ota or update manager) for secure firmware updates.

Build & Flash (PlatformIO)

1. Install PlatformIO in VS Code. Ensure the `platformio` CLI is available in your PATH.
2. Open this folder in VS Code and connect your ESP32 device.
3. Build and upload with PlatformIO:

```powershell
platformio run --environment esp32dev
platformio run --target upload --environment esp32dev
```

## PowerShell PATH tip

If you need to use the PlatformIO CLI from PowerShell and you installed PlatformIO using the VS Code extension (which places the CLI into a per-user virtualenv), you can add the `penv\Scripts` folder to your current session PATH with this command:

```powershell
$env:Path += ";$env:USERPROFILE\.platformio\penv\Scripts"
```

After running that line you can run `platformio` directly in the same PowerShell session (for example: `platformio run --environment esp32dev`).

Unit testing / verification steps

1. Verify I2C and AHT10

   - Power the board and open the serial monitor (115200).
   - Observe boot logs. If I2C is wired properly the hourly task will attempt a read on boot and log stored readings.
   - Optionally add a temporary call to `aht10_read()` from `app_main` for immediate verification.

2. Verify PWM outputs

   - Use a multimeter or oscilloscope on the configured GPIOs (defaults: GPIO18 for light, GPIO19 for pump).
   - Connect via TCP (port 3333) and send `SET LIGHT 4095` (max for 13-bit resolution) to set full brightness.

3. Verify Wi‑Fi and time sync

   - Store Wi‑Fi credentials into NVS (via a helper or future BLE flow). For quick testing, use the `nvs_set_str()` functions in a small helper app or modify `app_main` to hardcode credentials.
   - When connected, SNTP will sync the time based on the timezone stored in NVS (key: `timezone`).

4. Verify scheduler

   - Ensure timezone/time is correct, then wait for the scheduled on/off time or adjust `on_hour`/`off_hour` keys in NVS for quick testing.

How to set NVS keys (quick helper using `idf.py menu` or code)

- You can add a small one-off function to write NVS keys at boot (SSID, password, timezone) while testing. Remove it after provisioning.

Requirements coverage

- PWM control for light and pump: Implemented using LEDC. (Done)
- Use ESP-IDF framework and PlatformIO: Project configured for espidf. (Done)
- I2C AHT10 readings: Implemented. (Done)
- BLE commission: Placeholder (TODO). (Deferred)
- Time sync via internet using location: SNTP implemented; timezone read from NVS. (Done)
- OTA updates: Placeholder (Deferred)
- PWM over Wi‑Fi using Flutter app: TCP control implemented; BLE app integration left to the app developer. (Partially Done)
- Hourly measurements stored in storage: Implemented using NVS ring buffer. (Done)
- Mobile app hourly sync over Wi‑Fi: App-side behavior; device exposes stored readings in NVS (future: implement HTTP endpoint or GATT characteristic). (Deferred)

Next steps

- Implement BLE GATT provisioning service to accept Wi‑Fi credentials, timezone, device name, and schedule.
- Add an HTTP or simple REST API to expose stored readings for the mobile app to sync with.
- Add OTA update flow and secure update verification.

