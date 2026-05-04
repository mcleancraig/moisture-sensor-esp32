# Garden Soil Moisture Sensor

Battery-powered wireless soil moisture sensors for garden and indoor plant use. Each unit reads capacitive soil moisture and battery level, publishing data over WiFi/MQTT to a home automation hub. Designed for long-term deployment — estimated 5+ months per charge on a 2000mAh 18650 cell.

Built around the Seeed XIAO ESP32-C6, configured entirely via a captive portal web interface with no hardcoded credentials. Integrates natively with Home Assistant via MQTT autodiscovery.

---

## Features

- Capacitive soil moisture reading (0–100%)
- Battery voltage and percentage monitoring
- WiFi provisioning via captive portal — no hardcoded credentials
- Static or DHCP IP addressing
- Home Assistant MQTT autodiscovery — sensors appear automatically
- Over-the-air (OTA) firmware updates
- Boot button reconfiguration — no reflashing needed to change settings
- Deep sleep between readings — ~15 minute cycle for long battery life
- Firmware version reported in every MQTT payload

---

## Wiring diagram

![Wiring diagram](wiring-diagram.svg)


The XIAO's onboard charger handles battery charging automatically when USB-C is connected. No external charge module is required.


---

## Hardware

### Per sensor

| Component | Detail |
|---|---|
| MCU | Seeed XIAO ESP32-C6 |
| Moisture sensor | HW-390 capacitive, 3-pin (VCC / GND / AOUT) |
| Battery | 18650 Li-ion, 2000mAh recommended |
| Reverse polarity protection | 1N5819 Schottky diode in series with battery positive |
| Battery voltage divider | 2× 200kΩ resistors (BAT+ → A0 → GND) |



---

## Software requirements

### Arduino IDE

Install via the Boards Manager: **esp32 by Espressif Systems**

Board selection: `XIAO_ESP32C6`

### Libraries

Install via the Library Manager:

- **PubSubClient** by Nick O'Leary

All other libraries (`WiFi`, `WebServer`, `DNSServer`, `Preferences`, `ArduinoOTA`, `ESPmDNS`) are included in the ESP32 Arduino core.

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
MOISTURE_XXXXXXXXXXXX
```

where `XXXXXXXXXXXX` is the device's unique MAC address.

**To configure:**

1. Connect to the `MOISTURE_` network — password is `moisture`
2. A setup page will appear automatically (captive portal). If it does not, open a browser and navigate to `192.168.4.1`
3. Fill in the form:
   - **Sensor number** — sets the sensor ID (`sensor1`, `sensor2` etc), friendly name, and default static IP last octet
   - **WiFi SSID and password** — your home network credentials
   - **Static IP** — optional. Check the box to assign a fixed IP address (recommended for reliable OTA updates). Defaults to `192.168.220.<sensor number>`
   - **MQTT broker address** — IP address of your MQTT broker (e.g. your Raspberry Pi)
   - **MQTT port** — default 1883
   - **MQTT username / password** — leave blank if your broker does not require authentication
4. Click **Save & Restart**

The sensor will restart, connect to your network, and begin reporting immediately.

If the portal is not used within 10 minutes it will close, the sensor sleeps for 10 minutes, then restarts and tries WiFi again. If WiFi fails on a configured sensor, the portal opens automatically so you can update credentials.

---

## Reconfiguring a deployed sensor

To change any setting (WiFi password, MQTT broker, sensor number etc) without reflashing:

1. Power the sensor on while holding the **boot button** on the XIAO board
2. Hold for 3 seconds until the serial monitor shows `Config — confirmed, clearing config`
3. Release the button
4. The sensor starts the configuration portal
5. Connect to the `MOISTURE_` network and reconfigure as above

A brief press (under 3 seconds) is ignored — normal boot continues.

---

## Home Assistant integration

The sensor uses MQTT autodiscovery. Once MQTT is configured in Home Assistant:

1. Go to **Settings → Devices & Services → MQTT**
2. Ensure **Enable discovery** is checked and the discovery prefix is `homeassistant`
3. On next sensor boot, a new device will appear automatically under **Settings → Devices**

Each sensor creates three entities in Home Assistant:

| Entity | Unit | Notes |
|---|---|---|
| Moisture | % | Soil moisture level |
| Battery | % | Estimated charge remaining |
| Battery Voltage | V | Raw cell voltage |

The device card also shows the firmware version (`sw_version`) on the device info page.

### MQTT topics

| Topic | Direction | Content |
|---|---|---|
| `garden/sensor1/state` | Sensor → broker | JSON state payload |
| `homeassistant/sensor/sensor1_moisture/config` | Sensor → broker | HA discovery config (retained) |
| `homeassistant/sensor/sensor1_battery_v/config` | Sensor → broker | HA discovery config (retained) |
| `homeassistant/sensor/sensor1_battery_pct/config` | Sensor → broker | HA discovery config (retained) |

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
  "fw": "2.0.0"
}
```

---

## OTA firmware updates

After connecting to WiFi, each sensor listens for an OTA update for 5 seconds before going to sleep. To push an update:

1. Ensure you are on the same network as the sensor (VPN must be disconnected)
2. In Arduino IDE, go to **Tools → Port** — the sensor should appear as a network port named `sensor1` (or its configured ID)
3. Select that port
4. Click **Upload** — the update will be pushed wirelessly

**OTA password:** `moisture`

If you miss the 5-second window, wait for the next wake cycle (up to 15 minutes) or hold the boot button to force a reconfiguration, which keeps the portal open longer.

**Note:** The serial monitor is not available over the OTA network port — switch back to the USB port to view serial output after an OTA flash.

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

Based on a 15-minute sleep cycle, 5-second OTA window, and 2000mAh cell:

| Phase | Current | Duration per cycle |
|---|---|---|
| Deep sleep | ~15µA | ~895 seconds |
| WiFi + MQTT | ~300mA peak | ~3 seconds |
| OTA window | ~80mA | 5 seconds |

Estimated runtime: **4–5 months** per charge. Actual runtime depends on WiFi signal strength, temperature, and cell quality.

The battery percentage reported uses a piecewise linear interpolation of a real 18650 discharge curve, giving accurate readings across the full voltage range rather than fixed steps.

---

## Factory reset

To completely clear all configuration and return to first-boot state, flash the `clear-config` sketch located in the `clear-config/` folder. This sketch lives in its own folder — open it as a separate sketch in Arduino IDE, flash it, then re-flash the main sketch.

---

## Deploying multiple sensors

Each sensor needs only one change — the sensor number is set during portal configuration and everything else derives from it automatically:

| Sensor number | Sensor ID | MQTT topic | Default static IP |
|---|---|---|---|
| 1 | sensor1 | garden/sensor1/state | 192.168.220.1 |
| 2 | sensor2 | garden/sensor2/state | 192.168.220.2 |
| 3 | sensor3 | garden/sensor3/state | 192.168.220.3 |

Sensor getting hot when USB connected
Check battery polarity. Overheating is caused by reverse polarity on the battery connection, not by USB and battery being connected simultaneously. USB and battery can safely coexist — the onboard charger is designed for this. Verify the 1N5819 diode is fitted with anode toward the battery positive terminal.
---

## Troubleshooting

**Sensor not appearing in Home Assistant**
Ensure MQTT integration is enabled in HA and discovery is turned on. Check the broker is reachable and credentials are correct by monitoring `garden/#` on the broker directly:
```bash
mosquitto_sub -h localhost -t "garden/#" -v -u YOUR_USER -P YOUR_PASSWORD
```

**OTA upload fails with "No response from device"**
- Ensure you are not connected to a VPN — OTA uses UDP which VPNs frequently block
- Check macOS firewall is not blocking Arduino IDE (System Settings → Network → Firewall → Options)
- Confirm you are on the same network segment as the sensor

**Moisture reading stuck at 0% or 100%**
Recalibrate — the `DRY_MV` and `WET_MV` values in the firmware need to match your specific sensor unit. Check `moisture_raw_mv` in the MQTT payload and compare against your calibration readings.

**Battery reading shows null**
The battery reading falls outside the sanity check range (2.5V–4.3V). Check the voltage divider wiring — two 220kΩ resistors from BAT+ to A0 to GND. Confirm A0 is the pin connected to the midpoint, not A1.

**Sensor getting hot when USB connected**
Check battery polarity. Overheating is caused by reverse polarity on the battery connection, not by USB and battery being connected simultaneously. USB and battery can safely coexist — the onboard charger is designed for this. Verify the 1N5819 diode is fitted with anode toward the battery positive terminal.

---

## Licence

GPL V2 - see LICENSE.md
