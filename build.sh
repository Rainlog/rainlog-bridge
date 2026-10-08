#!/usr/bin/env bash
# Build with the pinned ESP-IDF Docker image. C6 is the default board.
#   ./build.sh
#   BOARD=lilygo ./build.sh
#   PORT=/dev/serial/by-id/<device> ./build.sh flash
#   BOARD=lilygo PORT=/dev/serial/by-id/<device> ./build.sh flash
#   BOARD=lilygo DEBUG_CONSOLE=1 ./build.sh
# Other arguments are passed to idf.py. Flash requires an explicit port.
set -euo pipefail

IMAGE=rainlog-wireless-bridge-idf
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
case "${BOARD:-c6}" in
  c6) default_radio=0; target=esp32c6; build_dir=build; config=sdkconfig; defaults=sdkconfig.defaults ;;
  lilygo) default_radio=1; target=esp32; build_dir=build-lilygo; config=sdkconfig.lilygo; defaults=sdkconfig.lilygo.defaults ;;
  *) echo "Unknown BOARD: use c6 or lilygo" >&2; exit 1 ;;
esac

case "${DEBUG_CONSOLE:-0}" in
  0) debug_console=OFF ;;
  1) debug_console=ON; build_dir+=-debug; config+=.debug ;;
  *) echo "DEBUG_CONSOLE must be 0 or 1" >&2; exit 1 ;;
esac

# Heap attribution changes allocation overhead, so keep audit builds separate.
case "${MEMORY_AUDIT:-0}" in
  0) ;;
  1)
    [[ "$debug_console" == ON ]] || { echo "MEMORY_AUDIT requires DEBUG_CONSOLE=1" >&2; exit 1; }
    build_dir+=-audit; config+=.audit; defaults+=";sdkconfig.audit.defaults"
    ;;
  *) echo "MEMORY_AUDIT must be 0 or 1" >&2; exit 1 ;;
esac

case "${RADIO:-$default_radio}" in
  0) radio=OFF ;;
  1) radio=ON ;;
  *) echo "RADIO must be 0 or 1" >&2; exit 1 ;;
esac

# No COPY instructions: don't send local demos, secrets or build caches.
docker build -t "$IMAGE" - < "$PROJECT_DIR/Dockerfile"

args=("$@")
[[ ${#args[@]} -gt 0 ]] || args=(build)
options=(--rm -v "$PROJECT_DIR":/project -e "IDF_TARGET=$target")
idf_args=(-D "RAINLOG_RADIO=$radio" -D "RAINLOG_DEBUG_CONSOLE=$debug_console" -B "$build_dir" -D "SDKCONFIG=/project/$config"
          -D "SDKCONFIG_DEFAULTS=/project/$defaults")
if [[ -n "${PORT:-}" ]]; then
  device=$(readlink -f "$PORT")
  options+=(--device "$device:$device")
  idf_args+=(-p "$device")
fi
for arg in "${args[@]}"; do
  case "$arg" in
    flash|app-flash|bootloader-flash|partition-table-flash|erase-flash|monitor)
      [[ -n "${PORT:-}" ]] || { echo "PORT is required for $arg" >&2; exit 1; }
      ;;
  esac
done
if [[ "${args[0]}" == flash && -t 0 && -t 1 ]]; then
  options+=(-it)
  args+=(monitor)
elif [[ " ${args[*]} " == *" monitor "* ]]; then
  options+=(-it)
fi
exec docker run "${options[@]}" "$IMAGE" idf.py "${idf_args[@]}" "${args[@]}"
