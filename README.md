ESP32 Grow Light & Air Pump Controller

This repository contains an ESP-IDF project implementing a secure, reliable controller with components:
- control: PWM LEDC control with soft-ramp
- storage: NVS storage with CRC + backup
- schedule: timezone-aware schedule handling
- net: Wi‑Fi, SNTP, BLE fallback arbitration
- ble: secure provisioning skeleton (ECDH + AEAD placeholders)
- telemetry: heartbeat and audit log publishing
- ota: OTA download skeleton (esp_https_ota)
- safety: watchdog and safe-shutdown stubs

Prerequisites
- Windows with ESP-IDF and toolchain installed
- Open an "ESP-IDF Terminal" (provided by VS Code extension) so environment variables are set

Build
Open the ESP-IDF Terminal in the workspace root (d:\github\esp32_light_and_pump) and run:

```powershell
idf.py set-target esp32
idf.py menuconfig   # adjust Kconfig options, pins, partitions
idf.py build
```

Flash (example)

```powershell
idf.py -p COM3 flash monitor
```

Run unit tests
Unit tests use Unity and the IDF test runner. To run tests for a component, use:

```powershell
# run all tests
idf.py test
# or run a single component's tests (example: control)
# idf.py test -C components/control
```

Notes & next steps
- Secure Boot v2, Flash Encryption, device X.509 cert population in `esp_secure_cert` partition, and AWS IoT mTLS require provisioning of keys/certs and extra build steps; these are intentionally left as manual steps for security.
- BLE: provisioning now uses ECDH + HKDF + AES-GCM and includes replay protection. A monotonic peer counter and a 64-bit sliding window are persisted in NVS at key `ble_peer_counter` / `ble_peer_window` to defend against replay across reboots.
Partition `esp_secure_cert` format (expected): PEM blobs concatenated in this order:

	1) CA / trusted signer certificate PEM
	2) Device client certificate PEM
	3) Device private key PEM

	The firmware will attempt to parse these PEMs at runtime and use them for:

	- Verifying AWS Job signatures (signer cert)
	- Verifying OTA image signatures before finalizing updates (signer cert)
	- Supplying client cert/key and root CA to the MQTT client for mTLS
- OTA: `ota_request_update(url, signature, sig_len)` now streams the firmware, computes SHA-256, verifies the hash against the provided signature using the signer cert from `esp_secure_cert`, and only finalizes the OTA when verification succeeds. The OTA path refuses unauthenticated images (signature required).
 - Important: You must provision `esp_secure_cert` with the signer CA/cert and device cert/key before enabling mTLS/OTA in production. There is no automatic provisioning of these secrets in this repository.


Manifest-based OTA
------------------
This repo supports a manifest-based OTA format. The manifest is JSON with these required fields:

- `url` - HTTPS URL to firmware binary
- `digest` - lowercase hex sha256 of the firmware binary (32 bytes -> 64 hex chars)
- `signature` - base64 ECDSA signature of the digest bytes (sha256)

When a job contains a `manifest` object, the firmware will verify the manifest signature against signer cert(s) in `esp_secure_cert`, then download the image and verify the image digest/signature before finalizing.


Provisioning helper
-------------------
Use `tools/provision_cert.py` to convert a PEM file into a partition image suitable for flashing to `esp_secure_cert`:

```powershell
python tools/provision_cert.py --pem device_combined.pem --out esp_secure_cert.bin --size 0x1000
# then flash using esptool.py (adjust offset per partitions.csv)
esptool.py --chip esp32 --port COM3 write_flash 0x10000 esp_secure_cert.bin
```

Unit tests
----------
There is a small unit test for BLE replay/window persistence at `components/ble/test/test_ble_replay.c`.
Run component tests with the IDF test runner: `idf.py test`.

Contact
- This code was generated as a starting point; review cryptography, key storage, and OTA signing before production use.
