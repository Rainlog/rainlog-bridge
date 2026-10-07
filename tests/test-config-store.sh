#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
# Copy the source so its quoted config.h include uses public test defaults,
# never the developer's ignored credentials.
cp main/config_store.c "$scratch/"
cp main/config.example.h "$scratch/config.h"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -DRAINLOG_RADIO=1 -DRAINLOG_RADIO_MHZ=433 -D CONFIG_IDF_TARGET_ESP32=1 -I tests/config-mocks -I tests/mocks -I main \
  tests/config_store_test.c "$scratch/config_store.c" -o "$scratch/test"
"$scratch/test"
echo 'Config validation, persistence and provisioning checks passed'
