#!/usr/bin/env bash
# Exercise both font choices on each geometry and SSD1306 page packing.
set -euo pipefail
cd "$(dirname "$0")/.."
bin_dir=$(mktemp -d)
trap 'rm -rf "$bin_dir"' EXIT
for target in ESP32C6 ESP32; do
  for font in BOARD_FONT_6X10 BOARD_FONT_8X13; do
    cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
      -D "CONFIG_IDF_TARGET_$target=1" -D "BOARD_DISPLAY_FONT=$font" -I tests/mocks -I main -I main/ui \
      tests/display_test.c main/ui/display.c -o "$bin_dir/$target-$font"
    ASAN_OPTIONS=detect_leaks=0 "$bin_dir/$target-$font" > "$bin_dir/strip-output"
    if [[ "$target" == ESP32C6 ]]; then
      cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
        -D "CONFIG_IDF_TARGET_$target=1" -D "BOARD_DISPLAY_FONT=$font" \
        -D DISPLAY_STRIP_ROWS=BOARD_DISPLAY_H -I tests/mocks -I main -I main/ui \
        tests/display_test.c main/ui/display.c -o "$bin_dir/full"
      ASAN_OPTIONS=detect_leaks=0 "$bin_dir/full" > "$bin_dir/full-output"
      diff -u "$bin_dir/full-output" "$bin_dir/strip-output"
    fi
    cat "$bin_dir/strip-output"
  done
done
