# Changelog

All notable changes to CLIMORA are documented here. This project follows [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Release and CI artifacts now include a SHA-256 checksum file for the compiled firmware binary, enabling independent integrity verification.

### Fixed
- Clarified the actual OTA validation guarantees so SHA-256 verification and `main`-only source enforcement are not documented as active protections before they are implemented.

## [5.5.0]

### Added
- Automatic OTA update checks for newer firmware releases.
- OTA progress and status visuals.
- Firmware build validation workflow.
