# TODO — v3 and beyond

Items identified during v2 development for future releases.

## Security

- [ ] **Per-device AP password** — derive the portal password from the last 3 bytes of the device MAC address instead of the shared `moisture` string. Print the password on a label affixed to the enclosure so each unit has a unique credential without any configuration overhead.

## Maintainability

- [ ] **MQTT-triggered OTA** — subscribe to a `garden/sensorN/ota` topic before sleeping. If a retained `1` payload is present, stay awake for an extended OTA window instead of the standard 5-second window. Eliminates the need to time uploads to the wake cycle and avoids adding unnecessary wake time to every normal cycle.

- [ ] **PCB design** — replace breadboard/perfboard wiring with a custom 2-layer PCB. Castellated footprint for the XIAO ESP32-C6, HW-390 JST connector, 1N5819 diode footprint, 2x 200k voltage divider resistors, 100nF bypass cap, BAT+/BAT- pads. Fabricate via JLCPCB. Reduces assembly time per unit from ~30 min to ~5 min.

## Scalability

- [ ] **Zigbee mesh migration** — the ESP32-C6 supports Zigbee 3.0 natively. For deployments beyond 15 sensors, or where battery life becomes a concern, reflash units as Zigbee end devices with one unit acting as coordinator (USB-powered, connected to Pi). Requires ESP-IDF rather than Arduino. Zigbee transmit current is significantly lower than WiFi, extending battery life considerably.

## Nice to have

- [ ] **Wiring diagram** — add a proper schematic diagram to the repo and README showing the complete circuit for the production hardware.

- [ ] **Photos** — add photos of the assembled unit, enclosure, and deployed sensor to the README.

- [ ] **Calibration helper** — add a calibration mode triggered by double-tap of the boot button that holds the sensor awake, streams raw ADC readings over serial, and publishes min/max observed values to MQTT to assist with DRY_MV / WET_MV calibration without reflashing.

- [ ] **Low battery alert** — publish to a `garden/sensorN/alert` topic when battery drops below 20%, allowing Home Assistant to trigger a notification.
