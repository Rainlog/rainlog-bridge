#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cc -D_POSIX_C_SOURCE=200809L -DRAINLOG_RADIO=1 -DRAINLOG_RADIO_MHZ=433 \
  -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main/radio \
  tests/rain_upload_test.c main/radio/rain_upload.c main/radio/weather_decode.c -lm -o "$scratch/test"
"$scratch/test"

cc -D_POSIX_C_SOURCE=200809L -DRAINLOG_RADIO=1 -DRAINLOG_RADIO_MHZ=433 \
  -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I tests/radio-upload-mocks -I tests/config-mocks -I main -I main/radio \
  tests/radio_upload_poll_test.c main/radio/rain_upload.c main/radio/radio_upload.c main/radio/weather_decode.c \
  -lm -o "$scratch/poll-test"
"$scratch/poll-test"
