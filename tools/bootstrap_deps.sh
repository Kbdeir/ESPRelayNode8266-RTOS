#!/usr/bin/env bash
set -euo pipefail
mkdir -p components/littlefs
if [ ! -d components/littlefs/littlefs/.git ]; then
  git clone https://github.com/littlefs-project/littlefs.git components/littlefs/littlefs
  git -C components/littlefs/littlefs checkout v2.11.3
fi
if [ ! -d components/mqtt/.git ]; then
  git clone https://github.com/espressif/esp-mqtt.git components/mqtt
  git -C components/mqtt checkout ESP-MQTT_FOR_IDF_3.1
fi
