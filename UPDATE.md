# Updating CLIMORA

## Automatic firmware updates

CLIMORA firmware checks **GitHub `main` only** for an approved update manifest. It must not automatically update from feature branches, development branches, tags, or release assets.

The device should only install firmware after the configured update manifest identifies a newer version and the firmware passes the updater's validation checks. If the check or download fails, the current firmware continues running.

## Manual update

Manual updates are performed by compiling the firmware from the repository's `main` branch and flashing it over USB using the existing OTA-capable partition scheme.

Before flashing, compile the sketch with the declared ESP32 board configuration and required libraries:

- ESP32 Arduino core
- FastLED
- ArduinoJson
- `PartitionScheme=min_spiffs`

The GitHub Actions workflow provides the same repeatable build configuration for `main`.

## Recovery

If an OTA update fails validation or cannot be completed, do not erase the current installation. Reflash a known-good firmware build from `main` over USB when physical recovery is required.
