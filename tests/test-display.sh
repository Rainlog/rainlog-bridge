#!/usr/bin/env bash
# Exercise the real renderer on both geometries and SSD1306 page packing.
set -euo pipefail
cd "$(dirname "$0")/.."
bin_dir=$(mktemp -d)
trap 'rm -rf "$bin_dir"' EXIT
for target in ESP32C6 ESP32; do
  cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -D "CONFIG_IDF_TARGET_$target=1" -I tests/mocks -I main -I main/ui \
    tests/display_test.c main/ui/display.c -o "$bin_dir/$target"
  ASAN_OPTIONS=detect_leaks=0 "$bin_dir/$target"
done
