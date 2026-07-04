# TODO — v3 and beyond

Items identified during v2 development for future releases.

## Security

- [ ] **FOTA TLS certificate validation** — `checkForUpdate()` currently uses `client.setInsecure()`, which skips certificate verification. An MITM on the network path between the sensor and GitHub could serve a malicious firmware binary that the device would flash without question. Fix: embed the ISRG Root X1 CA certificate (~1.5 KB of flash) and call `client.setCACert(rootCA)` instead. This secures the entire download path without requiring manual cert rotation.

- [ ] **FOTA firmware integrity check** — even with TLS validation, publishing a SHA-256 hash of `firmware.bin` as a third release asset and verifying it before flashing would provide defence-in-depth against a compromised GitHub account or release. Requires downloading the binary manually rather than via `httpUpdate`, then verifying the hash before calling `Update.begin()`.

- [ ] **P-channel MOSFET reverse polarity protection** — replace the removed 1N5819 diode with a P-channel MOSFET (e.g. AO3401) wired as a reverse polarity switch. Unlike the diode this has near-zero voltage drop and allows bidirectional current, meaning charging works correctly while still protecting against reversed battery insertion. The 1N5819 diode blocked charging current and has been removed from the design pending this fix.

## Maintainability

- [x] **NVS magic — migrate pre-2.5.3 sensors (v2.11.0)** — v2.10.2's clear-on-absent tightening turned out to wipe sensors that had run pre-2.5.3 firmware and never since triggered a config save, so v2.11.0 reverted it: absent magic now proceeds normally and is written passively at the end of a successful `loadConfig()`, migrating those sensors on their next boot with no portal interaction. Wrong-value magic still clears NVS as before.

- [x] **FOTA (v2.1.0)** — sensors automatically check GitHub Releases on each wake and self-update. Attach `version.txt` and `firmware.bin` to each release. IDE-based OTA removed.

- [ ] **PCB design** — replace breadboard/perfboard wiring with a custom 2-layer PCB. Castellated footprint for the XIAO ESP32-C6, HW-390 JST connector, 2x 200k voltage divider resistors, 100nF bypass cap, BAT+/BAT- pads. Fabricate via JLCPCB. Reduces assembly time per unit from ~30 min to ~5 min.

## Power

- [ ] **Power-gate the soil moisture sensor** — the HW-390's VCC is wired directly to the 3V3 rail, drawing 5–10 mA continuously including during deep sleep. Fix: wire sensor VCC to a free GPIO (e.g. GPIO4), drive it HIGH just before the ADC read, wait ~80 ms to stabilise, sample, then pull LOW before sleeping. Reduces deep-sleep current from ~5000 µA to ~20 µA — a 99.6% saving. Add `moisturePowerPin` as a config field (portal + NVS) so the pin can be set per-unit without reflashing.

## Scalability

- [ ] **Zigbee mesh migration** — the ESP32-C6 supports Zigbee 3.0 natively. For deployments beyond 15 sensors, or where battery life becomes a concern, reflash units as Zigbee end devices with one unit acting as coordinator (USB-powered, connected to Pi). Requires ESP-IDF rather than Arduino. Zigbee transmit current is significantly lower than WiFi, extending battery life considerably.

## Nice to have

- [x] **Wiring diagram** — SVG diagram in repo showing XIAO ESP32-C6, 18650, HW-390, voltage divider, and reed switch. Photos still outstanding (see below).

- [-] ~~**Reed switch testing**~~ *(skipped)* — two-stage hold (3 s = restart, 10 s = wipe config) implemented in v2.4.1 but untested on real hardware.

- [ ] **Photos** — add photos of the assembled unit, enclosure, and deployed sensor to the README.

- [ ] **Calibration helper** — add a calibration mode triggered by double-tap of the boot button that holds the sensor awake, streams raw ADC readings over serial, and publishes min/max observed values to MQTT to assist with DRY_MV / WET_MV calibration without reflashing.

- [x] **Low battery alert** — binary sensor autodiscovery entity, reports ON at ≤ 15% (v2.4.1).

- [x] **FOTA downgrade protection** — implemented in v2.9.0; `isNewerVersion()` uses numeric semver comparison and only updates when remote > local.
