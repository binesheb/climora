# CLIMORA

Natural weather ambience engine for a single 60-LED/m WS2812B ambience strip.

## Firmware

The firmware is designed for an ESP32 with an OTA-capable partition scheme.

### Automatic GitHub Release OTA

CLIMORA checks the latest published GitHub Release after Wi-Fi becomes available and then every 6 hours.

The updater:

- reads the latest published release from the GitHub Releases API
- looks for `climora-firmware.bin`
- compares semantic firmware versions
- validates the expected firmware size
- downloads the firmware over HTTPS
- calculates and verifies the GitHub-provided SHA-256 asset digest
- writes the firmware to the OTA partition
- reboots only after a successful update
- continues running the current firmware if the check/download/update fails

The first firmware installation must still be done by USB. After an OTA-capable CLIMORA firmware is installed, subsequent published releases can be installed automatically.

### Release process

Create a semantic version tag such as:

```text
v5.6.0
```

GitHub Actions compiles the ESP32 firmware using the OTA-capable **Minimal SPIFFS (1.9MB APP with OTA/190KB SPIFFS)** partition scheme and publishes `climora-firmware.bin` as the release asset.

The ESP32 then discovers that release automatically.

## Hardware

- ESP32
- WS2812B, 60 LEDs/m
- 60 LEDs configured
- Data pin: GPIO 5
- Status LED: GPIO 2
