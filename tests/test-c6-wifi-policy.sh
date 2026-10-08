#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cp main/config_store.c "$scratch/"
cp main/config.example.h "$scratch/config.h"
cc -std=c11 -Wall -Wextra -Werror -ffunction-sections -fdata-sections \
  -DRAINLOG_RADIO=0 -DRAINLOG_RADIO_MHZ=0 -DCONFIG_IDF_TARGET_ESP32C6=1 \
  -I tests/config-mocks -I tests/mocks -I main \
  tests/c6_wifi_policy_test.c "$scratch/config_store.c" -Wl,--gc-sections -o "$scratch/test"
"$scratch/test"
echo 'C6 always requires Bridge Wi-Fi and exposes no idle shutdown setting'
