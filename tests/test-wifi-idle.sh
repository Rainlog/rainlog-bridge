#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main tests/wifi_idle_test.c -o "$scratch/test"
"$scratch/test"
echo 'Bridge Wi-Fi idle boundary, connected clients, setup and wake checks passed'
