#!/usr/bin/env bash
set -e

SKETCH="moisture-sensor-esp32.ino"
FQBN="esp32:esp32:XIAO_ESP32C6:CDCOnBoot=cdc"

arduino-cli compile \
  --fqbn "$FQBN" \
  --output-dir build/ \
  "$SKETCH"

echo "Build complete. Binary: build/${SKETCH%.ino}.ino.bin"
