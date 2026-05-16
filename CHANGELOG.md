# Changelog

All notable changes to moisture-sensor-esp32 are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [2.9.0] — 2026-05-16

### Changed
- `sensorNumber` renamed to `unitNumber` in config struct for cross-repo consistency (NVS key `sensorNum` unchanged — no factory reset required)
- State payload field `"fw"` renamed to `"fw_version"` — update any HA value templates that reference `value_json.fw`
- FOTA version comparison now uses integer semver parsing — fixes `"2.10.0" < "2.9.0"` bug under lexicographic `strcmp`
- Beta channel FOTA now uses strict `isNewerVersion()` instead of `!=` — prevents downgrade if remote beta is older

### Added
- `"rssi"` field in state payload (WiFi RSSI in dBm)
- Full wake-reason switch: timer, GPIO, and unexpected causes all logged with reason codes
- `esp_reset_reason()` logged at boot — surfaces panics, WDT resets, and brownouts in syslog
- Heap monitoring: free + minimum free heap logged after WiFi connects and after FOTA check
- `validateConfig()` called at boot to warn of GPIO pin conflicts
- `"MQTT — HA discovery published"` log line after all discovery payloads sent
- MQTT connect log now includes port number
- `*.bin` added to `.gitignore`
- `CHANGELOG.md` and `build.sh`

## [2.8.0] — 2026-04-XX

### Added
- Beta FOTA channel — opt sensors into pre-releases via HA select entity
- Force-update via MQTT — `garden/sensorN/update` retained flag triggers immediate FOTA check on next wake
- HA button entity for firmware update

### Changed
- Sleep interval restored to 120 minutes
- FOTA moved after MQTT listen phase so force-update command takes effect same wake

## [2.7.0]

### Changed
- NTP re-sync skipped when RTC already holds valid time (saves ~8 s per wake)
- FOTA version check throttled to once per 24 h via RTC memory timestamp
- HA discovery publish skipped on normal deep-sleep wakes (republishes on cold boot or after 7 days)

## [2.6.2]

### Changed
- Sleep interval increased from 15 to 120 minutes

## [2.6.1]

### Fixed
- Reed switch default pin changed from GPIO3 to GPIO2 (GPIO3 non-functional as digital input on XIAO ESP32-C6)

## [2.6.0]

### Added
- Reed switch GPIO wakeup — device wakes within ms of magnet contact, enabling reliable 3 s/10 s hold timing

## [2.5.3]

### Added
- NVS magic key validation — detects stale config from foreign firmware and falls through to portal

[2.9.0]: https://github.com/mcleancraig/moisture-sensor-esp32/releases/tag/v2.9.0
[2.8.0]: https://github.com/mcleancraig/moisture-sensor-esp32/releases/tag/v2.8.0
