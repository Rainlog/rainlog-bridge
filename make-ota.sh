#!/usr/bin/env bash
# Build a firmware OTA release for the Rainlog Wireless Bridge and stage it for
# rainlog.org/rainlog-bridge-ota/ (served by the existing nginx).
#
#   ./make-ota.sh [OUTDIR]
#
# Builds the firmware (via build.sh, in Docker), then writes into OUTDIR:
#   - rainlog-bridge-<board>-<version>.bin  the app image
#   - manifest-<board>.json                 {version, board, url, sha256, ...}
# where <version> is PROJECT_VER from CMakeLists.txt and <board> is BOARD_ID
# from main/board.h (one image stream per flash variant).
#
# OUTDIR defaults to ./dist (gitignored). To publish, rsync its contents to the
# document root the web server exposes under /rainlog-bridge-ota/.
#
# The bridge polls https://rainlog.org/rainlog-bridge-ota/manifest-<board>.json,
# compares `version` to the running firmware, and downloads `url` if newer.
# Filenames are versioned + immutable, so old images can stay for rollback
# reference.
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "$0")" && pwd)"
OUTDIR="${1:-$PROJECT_DIR/dist}"
BASE_URL="https://rainlog.org/rainlog-bridge-ota"

cd "$PROJECT_DIR"

# Version is the single source of truth in CMakeLists.txt (also baked into
# esp_app_desc, which the firmware compares against the manifest).
VERSION="$(sed -n 's/^[[:space:]]*set(PROJECT_VER[[:space:]]*"\([^"]*\)").*/\1/p' \
  CMakeLists.txt)"
if [[ -z "$VERSION" ]]; then
  echo "make-ota: could not read PROJECT_VER from CMakeLists.txt" >&2
  exit 1
fi

# Board identity comes from main/board.h (also compiled into the firmware,
# which refuses a manifest whose "board" doesn't match, and derives its
# CFG_OTA_MANIFEST_PATH from it). One manifest + image stream per board variant,
# named for the BOARD_ID (the id encodes the flash size, since that fixes the
# partition layout): the 8MB build is esp32-c6fh8-lcd-1.47, a 4MB build would be
# esp32-c6fh4-lcd-1.47, etc.
BOARD="$(sed -n 's/^#define BOARD_ID "\([^"]*\)".*/\1/p' main/board.h)"
if [[ -z "$BOARD" ]]; then
  echo "make-ota: could not read BOARD_ID from main/board.h" >&2
  exit 1
fi
BIN_NAME="rainlog-bridge-$BOARD-$VERSION.bin"
MANIFEST_NAME="manifest-$BOARD.json"

echo "make-ota: building firmware v$VERSION for $BOARD"
./build.sh

BIN_SRC="$PROJECT_DIR/build/rainlog-wireless-bridge.bin"
if [[ ! -f "$BIN_SRC" ]]; then
  echo "make-ota: build output not found: $BIN_SRC" >&2
  exit 1
fi

mkdir -p "$OUTDIR"
cp "$BIN_SRC" "$OUTDIR/$BIN_NAME"

SHA256="$(sha256sum "$OUTDIR/$BIN_NAME" | cut -d' ' -f1)"
SIZE="$(wc -c < "$OUTDIR/$BIN_NAME" | tr -d ' ')"
DATE="$(date -u +%Y-%m-%dT%H:%M:%SZ)"

cat > "$OUTDIR/$MANIFEST_NAME" <<EOF
{
  "version": "$VERSION",
  "board": "$BOARD",
  "url": "$BASE_URL/$BIN_NAME",
  "sha256": "$SHA256",
  "size": $SIZE,
  "notes": "",
  "date": "$DATE"
}
EOF

echo "make-ota: staged release in $OUTDIR"
echo "  $BIN_NAME ($SIZE bytes)"
echo "  $MANIFEST_NAME -> $BASE_URL/$BIN_NAME"
echo "  sha256 $SHA256"
