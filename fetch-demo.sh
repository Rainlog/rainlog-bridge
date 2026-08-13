#!/usr/bin/env bash
# Download and extract the official Waveshare ESP32-C6-LCD-1.47 demo archive
# into ESP32-C6-LCD-1.47-Demo/ for reference (LCD/RGB-LED/WiFi example code).
#
# The archive is ~60MB (bundles an Arduino tree, the ESP-IDF demo with vendored
# LVGL/led_strip, and a prebuilt firmware .bin), so it is NOT committed: the
# whole ESP32-C6-LCD-1.47-Demo/ directory is gitignored except for .keep. Run
# this script to populate it locally.
#
#   ./fetch-demo.sh          download + extract (skips if already extracted)
#   ./fetch-demo.sh --force  re-download and overwrite
set -euo pipefail

DEMO_URL="https://files.waveshare.com/wiki/ESP32-C6-LCD-1.47/ESP32-C6-LCD-1.47-Demo.zip"
SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
DEST_DIR="$SCRIPT_DIR/ESP32-C6-LCD-1.47-Demo"
ZIP_PATH="$DEST_DIR/ESP32-C6-LCD-1.47-Demo.zip"

force=0
[[ "${1:-}" == "--force" ]] && force=1

if [[ $force -eq 0 && -d "$DEST_DIR/ESP-IDF" ]]; then
  echo "Demo already extracted in $DEST_DIR (ESP-IDF/ present). Use --force to refresh."
  exit 0
fi

mkdir -p "$DEST_DIR"
echo "Downloading demo archive (~60MB) from:"
echo "  $DEMO_URL"
curl -fSL --retry 3 -o "$ZIP_PATH" "$DEMO_URL"

echo "Extracting into $DEST_DIR ..."
unzip -oq "$ZIP_PATH" -d "$DEST_DIR"

# The archive is gitignored anyway; drop it after extraction to reclaim ~60MB.
rm -f "$ZIP_PATH"

echo "Done. Reference example is under:"
echo "  $DEST_DIR/ESP-IDF/ESP32-C6-LCD-1.47-Test/"
