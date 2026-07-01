#!/usr/bin/env bash
# Clears retained MQTT state left behind by v3.0.0's removal of the Restart
# button, Update-on-next-wake button, and Reed Switch Pin control. These are
# retained messages the broker holds onto forever once nothing republishes
# them — the firmware change alone does not clear them.
#
# Run once per sensor, after that sensor has updated to v3.0.0+.
#
# Usage: MQTT_HOST=broker MQTT_USER=user MQTT_PASS=pass ./cleanup-mqtt-retained.sh 1 2 3
set -e

HOST="${MQTT_HOST:-localhost}"
PORT="${MQTT_PORT:-1883}"
AUTH=()
[ -n "$MQTT_USER" ] && AUTH+=(-u "$MQTT_USER")
[ -n "$MQTT_PASS" ] && AUTH+=(-P "$MQTT_PASS")

if [ "$#" -eq 0 ]; then
  echo "Usage: MQTT_HOST=... MQTT_USER=... MQTT_PASS=... $0 <sensor number> [<sensor number> ...]" >&2
  exit 1
fi

for n in "$@"; do
  sensor="sensor$n"
  echo "Clearing retained v2 controls for $sensor..."
  mosquitto_pub -h "$HOST" -p "$PORT" "${AUTH[@]}" -t "garden/$sensor/update" -n --retain
  mosquitto_pub -h "$HOST" -p "$PORT" "${AUTH[@]}" -t "homeassistant/button/${sensor}_restart/config" -n --retain
  mosquitto_pub -h "$HOST" -p "$PORT" "${AUTH[@]}" -t "homeassistant/button/${sensor}_update/config" -n --retain
  mosquitto_pub -h "$HOST" -p "$PORT" "${AUTH[@]}" -t "homeassistant/number/${sensor}_cfg_reed_pin/config" -n --retain
done

echo "Done."
