# Updating CLIMORA

## Automatic firmware updates

CLIMORA checks the latest published GitHub Release for an approved `climora-firmware.bin` after Wi-Fi becomes available and then periodically. The device only considers a newer semantic version, validates the release asset size, validates the published `climora-firmware.bin.sha256` digest over TLS, and only then writes the OTA partition. If the check, certificate validation, checksum, download, size validation, or flash write fails, the current firmware continues running.

The checked-in updater does **not yet** enforce a `main`-only source policy: it currently requests GitHub's `releases/latest` endpoint. Do not treat a main-branch source policy as active until the firmware and release process are updated together.

## Manual update

Manual updates are performed by compiling the firmware from the repository's `main` branch and flashing it over USB using the existing OTA-capable partition scheme.

Before flashing, compile the sketch with the declared ESP32 board configuration and required libraries:

- ESP32 Arduino core
- FastLED
- ArduinoJson
- `PartitionScheme=min_spiffs`

The GitHub Actions workflow provides the same repeatable build configuration for `main` and version tags.

## Recovery

If an OTA update fails validation or cannot be completed, do not erase the current installation. Reflash a known-good firmware build from `main` over USB when physical recovery is required.
