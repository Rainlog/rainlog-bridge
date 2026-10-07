#!/usr/bin/env bash
# Build with the pinned ESP-IDF Docker image. C6 is the default board.
#   ./build.sh
#   BOARD=lilygo ./build.sh
#   PORT=/dev/serial/by-id/<device> ./build.sh flash
#   BOARD=lilygo PORT=/dev/serial/by-id/<device> ./build.sh flash
# Other arguments are passed to idf.py. Flash requires an explicit port.
set -euo pipefail

IMAGE=rainlog-wireless-bridge-idf
PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
case "${BOARD:-c6}" in
  c6) target=esp32c6; build_dir=build; config=sdkconfig; defaults=sdkconfig.defaults ;;
  lilygo) target=esp32; build_dir=build-lilygo; config=sdkconfig.lilygo; defaults=sdkconfig.lilygo.defaults ;;
  *) echo "Unknown BOARD: use c6 or lilygo" >&2; exit 1 ;;
esac

# No COPY instructions: don't send local demos, secrets or build caches.
docker build -t "$IMAGE" - < "$PROJECT_DIR/Dockerfile"

args=("$@")
[[ ${#args[@]} -gt 0 ]] || args=(build)
options=(--rm -v "$PROJECT_DIR":/project -e "IDF_TARGET=$target")
idf_args=(-B "$build_dir" -D "SDKCONFIG=/project/$config"
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
