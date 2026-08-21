# CLIMORA

Natural weather ambience engine for a single 60-LED/m WS2812B ambience strip.

## Firmware

The firmware is designed for an ESP32 with an OTA-capable partition scheme.

### Automatic GitHub Release OTA

The current OTA implementation checks the latest published GitHub Release after Wi-Fi becomes available and then every 6 hours.

The updater compares semantic versions, validates the expected asset size, downloads `climora-firmware.bin`, writes the OTA partition, and reboots only after a successful update. If GitHub or the download is unavailable, the existing firmware continues running.

> **Current implementation note:** release-asset SHA-256 verification and the documented `main`-only update policy are not yet enforced by the checked-in OTA code. These are tracked as follow-up hardening work and must not be represented as active protections until implemented.

The first installation of the OTA-capable firmware must still be done by USB. After that, subsequent approved updates can be installed automatically.

### Release process

Use Semantic Versioning tags such as `v5.6.0`. The repository workflow builds the ESP32 firmware with the OTA-capable **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** partition scheme. A release should only be published after the updater's source-policy and integrity validation requirements are satisfied.

### CI validation

Firmware changes on `main` are compiled automatically before release work proceeds.

## Hardware

- ESP32
- WS2812B, 60 LEDs/m
- 60 LEDs configured
- Data pin: GPIO 5
- Status LED: GPIO 2
