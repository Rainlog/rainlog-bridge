#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
TEST_BIN=$(mktemp)
trap 'rm -f "$TEST_BIN"' EXIT
cc -std=c11 -Wall -Wextra -Werror -I main/ota tests/firmware_image_test.c main/ota/firmware_image.c -o "$TEST_BIN"
"$TEST_BIN" "$@"
