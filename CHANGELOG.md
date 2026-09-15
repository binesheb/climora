# Changelog

All notable changes to CLIMORA are documented here. This project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Fixed
- Aligned OTA documentation with the implemented `main`-only release-source enforcement and firmware SHA-256 verification.

## [5.5.0]

### Added
- Automatic OTA update checks for newer firmware releases.
- OTA progress and status visuals.
- Firmware build validation workflow.
- Release and CI artifacts include a SHA-256 checksum file for the compiled firmware binary, enabling independent integrity verification.
