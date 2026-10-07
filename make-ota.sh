#!/usr/bin/env bash
# Build a firmware OTA release for the Rainlog Wireless Bridge and stage it for
# rainlog.org/rainlog-bridge-ota/ (served by the existing nginx).
#
#   ./make-ota.sh [OUTDIR]
#   BOARD=lilygo ./make-ota.sh [OUTDIR]
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

case "${BOARD:-c6}" in
  c6) BUILD_DIR=build ;;
  lilygo) BUILD_DIR=build-lilygo ;;
  *) echo "Unknown BOARD: use c6 or lilygo" >&2; exit 1 ;;
esac

echo "make-ota: building firmware v$VERSION for ${BOARD:-c6}"
./build.sh

# Preprocess the real board header against this build's generated sdkconfig.
# Reading every textual BOARD_ID definition would mix the two board branches.
BOARD_ID="$(printf '#include "board.h"\nBOARD_ID\n' | \
  docker run --rm -i --entrypoint cc -v "$PROJECT_DIR":/project \
    rainlog-wireless-bridge-idf -E -P -I /project/main \
    -I "/project/$BUILD_DIR/config" -x c - | tr -d '"[:space:]')"
if [[ ! "$BOARD_ID" =~ ^[a-zA-Z0-9._-]+$ ]]; then
  echo "make-ota: could not read compiled BOARD_ID" >&2
  exit 1
fi
BIN_NAME="rainlog-bridge-$BOARD_ID-$VERSION.bin"
MANIFEST_NAME="manifest-$BOARD_ID.json"
BIN_SRC="$PROJECT_DIR/$BUILD_DIR/rainlog-wireless-bridge.bin"
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
  "board": "$BOARD_ID",
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
