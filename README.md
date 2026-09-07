# CLIMORA

Natural weather ambience engine for a single 60-LED/m WS2812B ambience strip.

## Firmware

The firmware is designed for an ESP32 with an OTA-capable partition scheme.

### Automatic GitHub Release OTA

The current OTA implementation checks the latest published GitHub Release after Wi-Fi becomes available and then every 6 hours.

The updater compares semantic versions, validates the expected asset size, downloads `climora-firmware.bin`, validates the published `climora-firmware.bin.sha256` digest over TLS, writes the OTA partition, and reboots only after a successful update. If GitHub, the checksum, the download, or validation is unavailable or fails, the existing firmware continues running.

> **Current implementation note:** TLS certificate validation and firmware SHA-256 verification are enforced by the checked-in OTA code. The updater still follows the GitHub `releases/latest` source and does not yet enforce the documented `main`-only source policy; that remains follow-up hardening work and must not be represented as an active protection.

The first installation of the OTA-capable firmware must still be done by USB. After that, subsequent approved updates can be installed automatically.

### Release process

Use Semantic Versioning tags such as `v5.6.0`. The repository workflow builds the ESP32 firmware with the OTA-capable **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** partition scheme and publishes both `climora-firmware.bin` and its SHA-256 checksum. A release should only be published after the updater's source-policy requirements are satisfied.

### CI validation

Firmware changes on `main` are compiled automatically before release work proceeds.

## Hardware

- ESP32
- WS2812B, 60 LEDs/m
- 60 LEDs configured
- Data pin: GPIO 5
- Status LED: GPIO 2
