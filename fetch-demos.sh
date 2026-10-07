#!/usr/bin/env bash
# Fetch official reference demos for the Waveshare ESP32-C6 LCD and
# LILYGO T3 LoRa32 V1.6.1 (433 MHz SX1278). Downloads are gitignored.
#
#   ./fetch-demos.sh          fetch missing demos
#   ./fetch-demos.sh --force  re-download and overwrite both demos
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
force=0
case "${1:-}" in
  "") ;;
  --force) force=1 ;;
  *) echo "Usage: $0 [--force]" >&2; exit 1 ;;
esac

# Stage archives outside the destination so a failed download or extraction
# leaves previously fetched reference files intact.
staging=$(mktemp -d)
trap 'rm -rf "$staging"' EXIT

fetch_demo() {
  local name="$1" url="$2" archive_root="$3" marker="$4"
  local dest="$SCRIPT_DIR/$name"
  if [[ $force -eq 0 && -f "$dest/$marker" ]]; then
    echo "Demo already present: $name. Use --force to refresh."
    return
  fi
  echo "Downloading $name from $url"
  curl -fSL --retry 3 -o "$staging/$name.zip" "$url"
  mkdir -p "$staging/$name"
  unzip -oq "$staging/$name.zip" -d "$staging/$name"
  local extracted="$staging/$name/$archive_root"
  [[ -f "$extracted/$marker" ]]
  mkdir -p "$dest"
  cp -a "$extracted/." "$dest/"
  echo "Fetched $name into $dest"
}

fetch_demo ESP32-C6-LCD-1.47-Demo \
  https://files.waveshare.com/wiki/ESP32-C6-LCD-1.47/ESP32-C6-LCD-1.47-Demo.zip \
  . ESP-IDF/ESP32-C6-LCD-1.47-Test/CMakeLists.txt

fetch_demo LilyGo-LoRa-Series \
  https://codeload.github.com/Xinyuan-LilyGO/LilyGo-LoRa-Series/zip/refs/heads/master \
  LilyGo-LoRa-Series-master examples/ArduinoLoRa/LoRaReceiver/LoRaReceiver.ino

echo "LILYGO 433 MHz board: select T3_V1_6_SX1278 in the example utilities.h."
