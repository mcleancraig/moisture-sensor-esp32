# Changelog

All notable changes to moisture-sensor-esp32 are documented here.
Format follows [Keep a Changelog](https://keepachangelog.com/).

## [3.0.0-b03] — 2026-07-04

### Added
- **Wi-Fi Fast Connect Caching (Optimization 2)** — Implemented caching of successful Wi-Fi channel and BSSID in RTC SRAM. Speeds up subsequent connection attempts to ~200ms under optimal conditions, falling back to a full scan if fast connect fails within 4 seconds.
- **Deep Sleep GPIO Pull-up Hold (Optimization 4)** — Maintains the Reed switch internal pull-up during deep sleep to prevent spurious wakeups, using conditional compilation to support the ESP32-C6's single-pin hold mechanism.

### Changed
- **Secure FOTA Certificate Bundle (Optimization 3)** — Replaced `client.setInsecure()` with secure validation using ESP32's built-in root CA certificate bundle (`esp_crt_bundle_attach` via a `SecureClient` wrapper) to properly and safely validate HTTPS connections and redirects.

## [3.0.0] — 2026-07-01

### Removed
- **Restart MQTT/HA control** — the Restart button and `garden/sensorN/cmd` `restart` command are gone. No longer applicable now the device already restarts every wake as part of its normal sleep cycle. `reset` (full config wipe + portal) is unaffected.
- **Reed Switch Pin MQTT/HA control** — the "Reed Switch Pin" number entity and `config/set/reedPin` are gone. `reedPin` is now portal-only, matching `wifiSSID`/`unitNumber`. The reed switch itself (hold 3s to restart, hold 10s to wipe config, GPIO wakeup) is unchanged.

### Changed
- **"Update on next wake" button renamed "Force check on next wake"** — b01 removed this control on the theory that routine FOTA (which checks every wake) made it redundant. That check is throttled to once per 24h, though, and this button is the only way to force a check sooner — e.g. right after cutting a beta for a test device. Restored in b02 under the clearer name.

### Added
- **Sleep Interval MQTT/HA control** — `config/set/sleepMinutes` and a "Sleep Interval" number entity (1-720 min) let you change how long the sensor sleeps between wake cycles without reflashing. Defaults to the previous hardcoded 120 minutes.

### Upgrade notes
- After every sensor is running v3.0.0+, run `cleanup-mqtt-retained.sh` against your broker to clear the retained Restart button discovery config and the Reed Switch Pin discovery config — neither is republished or cleared automatically by the firmware change alone.

## [2.11.0] — 2026-07-01

### Fixed
- **NVS magic regression (v2.10.2)**: absent magic (sensors configured before v2.5.3 that never triggered a config change post-upgrade) now proceeds normally again instead of wiping NVS. Magic is written passively at the end of a successful `loadConfig()` so the migration happens on the next boot without any portal interaction. Wrong-value magic still wipes NVS as before.
- **First boot delay disabled after portal re-provision**: `handleSave()` was copying `firstBootDelayMin` from `cfg` even when `loadConfig()` had returned early (after a magic wipe), leaving `cfg` zero-initialised and silently saving `firstBootDelayMin = 0`. Now uses the 15-minute default when config was never loaded, preserving an explicit user value when it was.

### Changed
- **Portal AP name**: shortened from full 12-char MAC (`MOISTURE_AABBCCDDEEFF`) to last-3-bytes short form (`MOISTURE-DDEEFF`) — unique per device, much easier to distinguish when multiple sensors are in portal mode simultaneously.
- **Boot logging**: `reset_reason` now includes human-readable string (`POWERON`, `SW`, `PANIC`, `WDT`, `BROWNOUT`, …). A new log line on cold boots and portal-restarts confirms whether the first boot delay is active and the configured value.
- **Sleep log reliability**: `goToSleep()` now waits 50 ms after logging before calling `esp_deep_sleep_start()`, giving the final syslog UDP packet time to transmit before the radio shuts down.

## [2.10.2] — 2026-05-27

### Changed
- NVS magic check tightened: absent magic key now clears the namespace and falls through to portal, matching the behaviour for a wrong-value magic key. Previously, absent magic was left alone for backwards compatibility with pre-2.5.3 sensors. All deployed sensors have run 2.5.3+ for some time.

## [2.10.1] — 2026-05-27

### Fixed
- `handleSave()` built `Config c = {}` (zero-initialised), saving `firstBootDelayMin = 0` to NVS and discarding the 15-minute default. Fixed: `handleSave()` now copies `firstBootDelayMin` from `cfg` before saving.
- Post-portal `ESP.restart()` gives `reset_reason = SW (3)`, not `POWERON (1)`, so the first operational boot after configuring a new sensor never triggered the delay. Fixed with a one-shot NVS flag (`firstBootPend`) set by `handleSave()` and atomically read and cleared by `checkAndClearFirstBootPending()` at next boot.

## [2.10.0] — 2026-05-27

### Added
- **First boot delay** — on power-cycle (`ESP_RST_POWERON`) or immediately after portal configuration, the device publishes all stats except moisture then sleeps for `firstBootDelayMin` (default 15 minutes) before waking for the first full reading. Prevents a spurious near-zero moisture reading while the sensor is still in open air after battery insertion.
- `firstBootDelayMin` config field (NVS key `firstBootMin`, default 15, range 0–120 minutes). Set to 0 to disable.
- HA `First Boot Delay` number entity (0–120 min) via autodiscovery.
- MQTT remote config: `config/set/firstBootDelayMin` — same pattern as all other configurable fields.

### Changed
- `goToSleep()` now takes an explicit `int minutes` parameter (was hardcoded to `SLEEP_MINUTES`).

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

## [2.8.0] — 2026-05-15

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

[2.11.0]: https://github.com/mcleancraig/moisture-sensor-esp32/releases/tag/v2.11.0
[2.10.2]: https://github.com/mcleancraig/moisture-sensor-esp32/releases/tag/v2.10.2
[2.10.1]: https://github.com/mcleancraig/moisture-sensor-esp32/releases/tag/v2.10.1
[2.10.0]: https://github.com/mcleancraig/moisture-sensor-esp32/releases/tag/v2.10.0
[2.9.0]: https://github.com/mcleancraig/moisture-sensor-esp32/releases/tag/v2.9.0
[2.8.0]: https://github.com/mcleancraig/moisture-sensor-esp32/releases/tag/v2.8.0
