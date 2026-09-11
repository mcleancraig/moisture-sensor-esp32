# Garden Soil Moisture Sensor

Battery-powered wireless soil moisture sensors for garden and indoor plant use. Each unit reads capacitive soil moisture and battery level, publishing data over WiFi/MQTT to a home automation hub. Designed for long-term deployment — estimated 5+ months per charge on a 2000mAh 18650 cell.

Built around the Seeed XIAO ESP32-C6, configured entirely via a captive portal web interface with no hardcoded credentials. Integrates natively with Home Assistant via MQTT autodiscovery.

---

## Features

- Capacitive soil moisture reading (0–100%)
- Battery voltage and percentage monitoring
- WiFi provisioning via captive portal — no hardcoded credentials
- Static or DHCP IP addressing, with configurable gateway, subnet and DNS
- Home Assistant MQTT autodiscovery — sensors appear automatically, autodiscovery republishes daily to recover from broker restarts
- Automatic firmware updates via FOTA from GitHub Releases, with rollback guard after 3 failed boots
- Beta FOTA channel — switch individual sensors between stable and beta firmware streams via Home Assistant
- Sensor power-gating — GPIO cuts HW-390 power during deep sleep, eliminating standby drain
- Syslog — structured UDP syslog to a configurable host for centralised logging
- Boot button reconfiguration — no reflashing needed to change settings
- MQTT command support — send `reset` remotely via retained message
- Deep sleep between readings — configurable interval (default 120 minutes) for long battery life
- First boot delay — skips moisture on first wake (default 15 minutes) so an open-air reading isn't sent while planting
- NTP timestamp in every MQTT payload
- Firmware version reported in every MQTT payload

---

## Wiring diagram

![Wiring diagram](wiring-diagram.svg)

---

## Hardware

### Per sensor

| Component | Detail |
|---|---|
| MCU | Seeed XIAO ESP32-C6 |
| Moisture sensor | HW-390 capacitive, 3-pin (VCC / GND / AOUT) |
| Battery | 18650 Li-ion, 2000mAh recommended |
| Reverse polarity protection | None currently — P-channel MOSFET planned (see TODO) |
| Battery voltage divider | 2x 200kOhm resistors (BAT+ to A0 to GND) |
| Sensor power-gating | HW-390 VCC to D4 (GPIO4); configure `sensorPowerPin` in portal |

### Wiring

```
18650 (+) ──────────────────────────── XIAO BAT+ pad
                                        └── 200kOhm ── A0 ── 200kOhm ── GND

18650 (−) ────────────────────────────── XIAO BAT− pad ── GND

HW-390 VCC  ── XIAO D4 (GPIO4)    ← power-gated (see note)
HW-390 GND  ── GND
HW-390 AOUT ── XIAO A1 (GPIO1)
```

After first boot, set **Sensor Power Pin** to `4` in the config portal. The firmware drives D4 HIGH just before the ADC read and LOW before sleeping, so the HW-390 draws no current during deep sleep. Any free GPIO 0–10 can be used; D4 is recommended as it is otherwise unused.

If power-gating is not wired, connect HW-390 VCC to XIAO 3V3 instead and leave Sensor Power Pin at `-1` (default).

The XIAO's onboard charger handles battery charging automatically when USB-C is connected. USB and battery can be connected simultaneously — this is normal and required for charging.

**Note:** No reverse polarity protection is currently fitted. Always check battery orientation before connecting.

---

## Software requirements

### Arduino IDE

Install via the Boards Manager: **esp32 by Espressif Systems**

Board selection: `XIAO_ESP32C6`

### Libraries

Install via the Library Manager:

- **PubSubClient** by Nick O'Leary

All other libraries (`WiFi`, `WebServer`, `DNSServer`, `Preferences`, `HTTPUpdate`, `WiFiClientSecure`, `ESPmDNS`) are included in the ESP32 Arduino core.

---

## Building and flashing

1. Open `moisture-sensor-esp32.ino` in Arduino IDE
2. Select board: **Tools → Board → XIAO_ESP32C6**
3. Connect the sensor via USB-C
4. Select the USB port under **Tools → Port**
5. Click **Upload**

No configuration changes are needed before flashing — all settings are entered via the captive portal after first boot.

---

## First-time setup

On first boot (or after a factory reset), the sensor starts a WiFi access point named:

```
MOISTURE-AABBCC
```

where `AABBCC` is the last three bytes of the device's MAC address in hex.

**To configure:**

1. Connect to the `MOISTURE-` network — password is `moisture`
2. A setup page will appear automatically (captive portal). If it does not, open a browser and navigate to `192.168.4.1`
3. Fill in the form:
   - **Sensor number** — sets the sensor ID (`sensor1`, `sensor2` etc), friendly name, and default static IP last octet
   - **WiFi SSID and password** — your home network credentials
   - **Static IP** — optional. Check the box to assign a fixed IP address. Defaults to `192.168.220.<sensor number>`. When enabled, also configure gateway, subnet mask and DNS
   - **MQTT broker address** — IP address of your MQTT broker (e.g. your Raspberry Pi)
   - **MQTT port** — default 1883
   - **MQTT username / password** — leave blank if your broker does not require authentication
4. Click **Save & Restart**

The sensor will restart, connect to your network, and begin reporting immediately.

**First boot delay:** on the first wake after power-on or portal save, the sensor skips the moisture reading and sleeps for 15 minutes before taking its first full reading. This prevents a misleading open-air reading being sent to Home Assistant while you're getting the sensor into the soil. Battery stats are still published on that first wake. The delay is configurable (0–120 minutes) via the **First Boot Delay** entity in Home Assistant, or disabled by setting it to 0.

If the portal is not used within 10 minutes it will close, the sensor sleeps for 10 minutes, then restarts and tries WiFi again. If WiFi fails on a configured sensor, the portal opens automatically so you can update credentials.

---

## Reconfiguring a deployed sensor

### Boot button (physical access required)

1. Power the sensor on while holding the **boot button** on the XIAO board
2. Hold for 3 seconds until the serial monitor shows `Config — confirmed, clearing config`
3. Release the button — the sensor starts the configuration portal

A brief press under 3 seconds is ignored — normal boot continues.

### MQTT command (fully remote)

Publish a retained message to `garden/sensorN/cmd`. The sensor receives it on its next wake cycle, self-clears the retained message, then acts on it:

```bash
# Reset config and open portal:
mosquitto_pub -h localhost -u USER -P PASS \
  -t "garden/sensor1/cmd" -m "reset" --retain

# Cancel a pending command:
mosquitto_pub -h localhost -u USER -P PASS \
  -t "garden/sensor1/cmd" -m "" --retain
```

From Home Assistant **Developer Tools → Actions → mqtt.publish**:

```yaml
topic: garden/sensor1/cmd
payload: reset
retain: true
```

---

## Home Assistant integration

The sensor uses MQTT autodiscovery. Once MQTT is configured in Home Assistant:

1. Go to **Settings → Devices & Services → MQTT**
2. Ensure **Enable discovery** is checked and the discovery prefix is `homeassistant`
3. On next sensor boot, a new device will appear automatically under **Settings → Devices**

Each sensor creates these entities in Home Assistant:

**Sensors**

| Entity | Unit | Notes |
|---|---|---|
| Moisture | % | Soil moisture level |
| Battery | % | Estimated charge remaining |
| Battery Voltage | V | Raw cell voltage |
| Last Seen | — | NTP timestamp of last reading (UTC) |
| Firmware Version | — | Running firmware version |
| RSSI | dBm | WiFi signal strength at time of reading |
| Battery Low | — | Binary sensor — ON when battery ≤ 15% |

**Controls**

| Entity | Type | Notes |
|---|---|---|
| Reset Config | Button | Clears NVS and opens captive portal on next wake |
| Force check on next wake | Button | Bypasses the 24 h FOTA throttle — triggers an immediate update check |
| Sleep Interval | Number (1–720 min) | Wake cycle duration; default 120 min |
| First Boot Delay | Number (0–120 min) | Delay before first moisture read; default 15 min |
| Update Channel | Select (stable/beta) | FOTA release stream for this sensor |
| Static IP | Switch | Enable/disable static IP addressing |
| MQTT Broker / Port / Username / Password | Text/Number | Remote MQTT config update |
| Syslog Host / Port | Text/Number | Remote syslog config update |
| IP Address / Gateway / Subnet Mask / DNS | Text | Remote network config update |
| Moisture Pin / Battery Pin / Sensor Power Pin | Number | GPIO pin assignments |

The device card also shows the firmware version (`sw_version`) on the device info page.

### MQTT topics

| Topic | Direction | Content |
|---|---|---|
| `garden/sensor1/state` | Sensor → broker | JSON state payload |
| `garden/sensor1/cmd` | Broker → sensor | Command: `reset` (retained) |
| `homeassistant/sensor/sensor1_moisture/config` | Sensor → broker | HA discovery config (retained) |
| `homeassistant/sensor/sensor1_battery_pct/config` | Sensor → broker | HA discovery config (retained) |
| `homeassistant/sensor/sensor1_battery_v/config` | Sensor → broker | HA discovery config (retained) |
| `homeassistant/sensor/sensor1_ts/config` | Sensor → broker | HA discovery config (retained) |
| `homeassistant/sensor/sensor1_fw/config` | Sensor → broker | HA discovery config (retained) |
| `homeassistant/sensor/sensor1_rssi/config` | Sensor → broker | HA discovery config (retained) |
| `homeassistant/binary_sensor/sensor1_battery_low/config` | Sensor → broker | HA discovery config (retained) |
| `homeassistant/button/sensor1_reset/config` | Sensor → broker | HA discovery config (retained) |
| `homeassistant/button/sensor1_update_check/config` | Sensor → broker | HA discovery config (retained) |
| `homeassistant/select/sensor1_fw_channel/config` | Sensor → broker | HA discovery config (retained) |
| `garden/sensor1/update` | Broker → sensor | Retained `1` — forces FOTA check on next wake |

### State payload example

```json
{
  "sensor": "sensor1",
  "moisture": 65,
  "moisture_raw_mv": 1843,
  "dry_mv": 2800,
  "wet_mv": 1000,
  "battery_v": 3.87,
  "battery_pct": 62,
  "battery_raw_mv": 1935,
  "rssi": -67,
  "fw_version": "3.0.2-b01",
  "ts": "2026-05-04T14:32:07Z"
}
```

---

## Firmware updates (FOTA)

On each wake the sensor checks GitHub Releases for a newer firmware version and updates itself automatically. No IDE or physical access needed.

**To release a firmware update:**

1. Bump `FIRMWARE_VERSION` in the sketch
2. Run `build.sh` to compile — output is `build/moisture-sensor-esp32.ino.bin`
3. Create a plain text file `version.txt` containing just the new version number (e.g. `3.0.2`) with no trailing newline
4. Create a new GitHub release tagged `vX.X.X`, attach both `moisture-sensor-esp32.ino.bin` and `version.txt` as release assets
5. Every sensor will pick up the update automatically on its next wake cycle

The `fw_version` field in the MQTT payload confirms the running firmware version. If an update fails the sensor continues operating on the existing firmware and retries on the next wake.

**FOTA rollback guard:** after a FOTA flash, the firmware counts consecutive failed boots (defined as wakes that do not complete a successful MQTT publish). After 3 failures it switches back to the previous OTA partition and logs `FOTA — ROLLED BACK` via syslog. The rejected version is blocked from re-downloading.

**Update Channel:** each sensor can independently follow the `stable` channel (GitHub Releases marked *Latest*) or the `beta` channel (most recent release including pre-releases). Switch a sensor by changing its **Update Channel** entity in Home Assistant — the new value is written to NVS on next wake. Default is `stable`.

**Force check on next wake:** pressing the **Force check on next wake** button in HA publishes a retained `1` to `garden/sensorN/update`. The sensor reads this on its next wake, clears the retained message, and runs a FOTA check immediately regardless of the 24 h throttle.

**Tagging and releasing from the command line:**

```bash
git tag -a v3.0.3 -m "v3.0.3 release notes here"
git push origin v3.0.3
```

Then on GitHub go to **Releases → Draft a new release**, select the tag, and attach `moisture-sensor-esp32.ino.bin` and `version.txt`.

---

## Beta releases

Riskier changes can be shipped to a subset of sensors first via the **beta FOTA channel** before promoting to the whole fleet. The two channels read different sources, which is why their release assets differ:

| Channel | Reads | Assets required |
|---|---|---|
| `stable` | `/releases/latest/download/version.txt`, then the binary from the same release | `moisture-sensor-esp32.ino.bin` **and** `version.txt` |
| `beta` | `tag_name` of the newest release via the GitHub API, including pre-releases | `moisture-sensor-esp32.ino.bin` only |

**To cut a beta:**

1. On the version branch (not `main`), set `FIRMWARE_VERSION` to `X.Y.Z-bNN` — e.g. `3.0.3-b01`; bump `NN` for a respin
2. Tag and push: `git tag -a v3.0.3-b01 -m "..." && git push origin v3.0.3-b01`
3. Draft a release from that tag, tick **Set as a pre-release**, and attach the `.bin`. No `version.txt` needed — the beta channel never reads it, and `/releases/latest` skips pre-releases so stable sensors are unaffected
4. Sensors with **Update Channel** set to `beta` pick it up on their next wake

**To promote a confirmed beta to stable:**

1. On the version branch, drop the `-bNN` suffix from `FIRMWARE_VERSION`
2. Finalise the `CHANGELOG.md` entry
3. Open the final PR from the version branch into `main` and merge
4. Tag and release as normal (`vX.Y.Z`, *not* a pre-release), attaching both the `.bin` and `version.txt`

`isNewerVersion()` treats a stable build as newer than a beta of the same version, and a higher `-bNN` as newer than a lower one, so promoting to stable is picked up automatically and switching a sensor's channel back and forth needs no version bump.

**Attach the binary before publishing, not after.** The beta channel bypasses the 24 h FOTA throttle, so a published pre-release with no `.bin` attached leaves beta sensors retrying a 404 on *every* wake.

**Verify the binary before uploading** — confirm the version baked into it matches the tag:

```bash
grep -a -o '3\.0\.3[^"]*' build/moisture-sensor-esp32.ino.bin | sort -u
```

A mismatch here means every sensor will download the "new" firmware, boot into the old version, see an update available again, and reflash indefinitely.

**Beta sensors park when no pre-release is newest.** If the most recent release is a stable one, beta-channel sensors log `FOTA — no beta release available yet, staying put` and hold their current build rather than moving to the stable. To bring them back in line with the fleet, switch their **Update Channel** to `stable`.

---

## Syslog

The sensor buffers log messages during its wake cycle and flushes them over UDP syslog after WiFi connects. This gives centralised, structured logging without a serial connection.

**Configuration:** set `Syslog Host` and `Syslog Port` in the captive portal, or update them remotely via the corresponding HA entities. Default port is `514`. Leave `Syslog Host` blank to disable syslog.

**Behaviour:**
- All `logf()` calls are buffered in RAM during boot and WiFi connection
- After WiFi connects the buffer is flushed: each entry is sent as a syslog UDP packet to the configured host
- If DNS resolution fails for the syslog host the buffer is discarded and syslog is skipped for that cycle (no startup delay)
- After flush, any subsequent log calls are sent immediately

**Useful log patterns:**
- `FOTA      — ROLLED BACK` — firmware rollback guard fired; investigate what changed
- `Discovery — skipped (3.0.3, NNm ago)` — discovery republish throttled; expected on most wakes
- `FOTA      — forced check` — triggered by the Force check HA button
- `WiFi      — connected, IP: … (312ms, cached)` — association time, and whether the cached BSSID or a full scan was used. A sensor logging `full scan` every wake is not benefiting from the connection cache
- `WiFi      — fast connect failed, retrying full scan connect` — the cached AP moved channel or went away; self-corrects, but persistent occurrences suggest an unstable AP or roaming between mesh nodes
- `Sleep     — awake NNNNms this wake` — total awake duration. The single most useful number for tracking per-wake energy use across the fleet
- `MQTT      — listen window closed after NNNms (N message(s) this wake)` — confirms remote commands are arriving inside the listen window

---

## Moisture sensor calibration

The HW-390 sensor must be calibrated once per unit for accurate percentage readings. The raw millivolt values are included in every MQTT payload (`moisture_raw_mv`) to assist with this.

**Calibration procedure:**

1. Flash the firmware and configure the sensor
2. Hold the sensor in open air — note the stable `moisture_raw_mv` value. Set this as `DRY_MV` in the firmware (typically ~2800mV)
3. Submerge the sensor to the white line marked on the PCB — note the stable value. Set this as `WET_MV` (typically ~1000mV)
4. Reflash with updated values

The HW-390 is inverted: dry soil reads a higher millivolt value than wet soil. `DRY_MV` should always be greater than `WET_MV`.

---

## Battery life

Based on a 120-minute sleep cycle (default) and 2000mAh cell:

| Phase | Current | Duration per cycle |
|---|---|---|
| Deep sleep | ~15uA | ~7,195 seconds |
| WiFi + MQTT | ~300mA peak | ~3 seconds |
| FOTA version check | ~80mA | ~2 seconds (no download if firmware is current) |

Estimated runtime: **5+ months** per charge. Actual runtime depends on WiFi signal strength, temperature, and cell quality.

The awake duration above is an estimate. Since v3.0.3 the real figure is logged every wake as `Sleep — awake NNNNms this wake`, so per-sensor energy use can be measured from syslog rather than assumed — worth checking against this table before making further power optimisations. Note that at a 120-minute interval, standby losses (deep sleep current, the 400 kΩ battery divider, regulator quiescent draw and cell self-discharge) are a substantial share of the budget, so reducing awake time has diminishing returns compared with cutting standby draw.

The battery percentage uses a piecewise linear interpolation of a real 18650 discharge curve, giving accurate readings across the full voltage range rather than fixed steps.

---

## Factory reset

To completely clear all configuration and return to first-boot state:

- **Via MQTT:** send a retained `reset` command (see Reconfiguring a deployed sensor above) — no physical access needed
- **Via boot button:** power the sensor on while holding the boot button for 3 seconds — the firmware clears NVS and starts the configuration portal. No reflashing required

---

## Deploying multiple sensors

Each sensor needs only one change — the sensor number is set during portal configuration and everything else derives from it automatically:

| Sensor number | Sensor ID | MQTT topic | Default static IP |
|---|---|---|---|
| 1 | sensor1 | garden/sensor1/state | 192.168.220.1 |
| 2 | sensor2 | garden/sensor2/state | 192.168.220.2 |
| 3 | sensor3 | garden/sensor3/state | 192.168.220.3 |

Each sensor must be calibrated individually as HW-390 units vary slightly from one another.

---

## Troubleshooting

**Sensor not appearing in Home Assistant**
Ensure MQTT integration is enabled in HA and discovery is turned on. Check the broker is reachable and credentials are correct by monitoring `garden/#` on the broker directly:

```bash
mosquitto_sub -h localhost -t "garden/#" -v -u YOUR_USER -P YOUR_PASSWORD
```

**FOTA update not applying**
- Ensure the sensor has internet access — verify DNS is configured correctly in the portal
- Check the `fw_version` field in the MQTT payload to confirm what version is currently running, and the sensor's **Update Channel** to confirm which stream it is following
- *Stable channel:* confirm `version.txt` is attached to the latest GitHub release and contains the correct version string with no leading `v` and no trailing whitespace or newlines. A leading `v` will not parse and the sensor will never update
- *Beta channel:* confirm the newest release is marked as a pre-release. If the newest release is a stable one, beta sensors deliberately stay put — see [Beta releases](#beta-releases)

**Sensor reflashing on every wake**
The version baked into the released binary does not match the release it is attached to, so the sensor downloads it, boots reporting the old version, and immediately sees an update available again. Check syslog for a repeating `FOTA — update available: X -> Y` where `X` never changes, then verify the release asset as described under [Beta releases](#beta-releases) and re-upload a correctly built binary. Sensors recover on their own once the corrected asset is in place.

**Moisture reading stuck at 0% or 100%**
Recalibrate — the `DRY_MV` and `WET_MV` values in the firmware need to match your specific sensor unit. Check `moisture_raw_mv` in the MQTT payload and compare against your calibration readings.

**Battery reading shows null**
The battery reading falls outside the sanity check range (2.5V–4.3V). Check the voltage divider wiring — two 200kOhm resistors from BAT+ to A0 to GND. Confirm A0 is the pin connected to the midpoint, not A1.

**Sensor getting hot when USB connected**
Check battery polarity. Overheating is caused by reverse polarity on the battery connection, not by USB and battery being connected simultaneously. USB and battery can safely coexist — the onboard charger is designed for this. No polarity protection is currently fitted — always check battery orientation carefully before connecting.

---

## Licence

GPL V2 — see LICENSE.md
