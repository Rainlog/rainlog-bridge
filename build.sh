#!/usr/bin/env bash
# Build (and optionally flash) the Rainlog Wireless Bridge entirely inside the
# pinned ESP-IDF Docker image. No host toolchain required.
#
#   ./build.sh              build the firmware
#   ./build.sh flash        build + flash + serial monitor the plugged-in board
#   ./build.sh <idf args>   run an arbitrary idf.py command in the container
#
# Target board: Waveshare ESP32-C6-LCD-1.47, native USB-CDC. Override the port
# with PORT=/dev/ttyXXX (default /dev/ttyACM0).
set -euo pipefail

IMAGE=rainlog-wireless-bridge-idf
PORT="${PORT:-/dev/ttyACM0}"
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"

docker build -t "$IMAGE" "$PROJECT_DIR"

cmd="${1:-build}"
case "$cmd" in
  build)
    # No TTY: plain build, works in CI / non-interactive shells.
    exec docker run --rm \
      -v "$PROJECT_DIR":/project \
      "$IMAGE" \
      idf.py build
    ;;
  flash)
    # Needs the USB device and an interactive TTY for the serial monitor.
    exec docker run --rm -it \
      -v "$PROJECT_DIR":/project \
      --device "$PORT":"$PORT" \
      "$IMAGE" \
      idf.py -p "$PORT" flash monitor
    ;;
  *)
    # Pass through any other idf.py invocation.
    exec docker run --rm -it \
      -v "$PROJECT_DIR":/project \
      --device "$PORT":"$PORT" \
      "$IMAGE" \
      idf.py "$@"
    ;;
esac
