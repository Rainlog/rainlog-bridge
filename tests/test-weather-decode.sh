#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
cc -DRAINLOG_RADIO=1 -DRAINLOG_RADIO_MHZ=433 -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -I main/radio \
  tests/weather_decode_test.c main/radio/weather_decode.c -o "$scratch/test"
"$scratch/test" tests/fixtures/tx5u-pulses.txt

# Unsupported bands and disabled radio builds must omit the 433 MHz decoders.
for variant in '1 915' '0 915' '0 433' '0 0'; do
  read -r enabled mhz <<< "$variant"
  cc -std=c11 -Wall -Wextra -Werror -I main/radio \
    -DRAINLOG_RADIO="$enabled" -DRAINLOG_RADIO_MHZ="$mhz" \
    -c main/radio/weather_decode.c -o "$scratch/disabled.o"
  if nm "$scratch/disabled.o" | rg -q 'weather_decode_(lacrosse|acurite)'; then
    echo "433 MHz decoder leaked into radio=$enabled band=$mhz" >&2
    exit 1
  fi
done

for variant in '1 433 1 0' '1 915 0 1' '0 433 0 0' '0 915 0 0' '0 0 0 0'; do
  read -r enabled mhz ook ambient <<< "$variant"
  cc -std=c11 -Wall -Wextra -Werror -I main/radio \
    -DRAINLOG_RADIO="$enabled" -DRAINLOG_RADIO_MHZ="$mhz" \
    -DEXPECT_OOK="$ook" -DEXPECT_AMBIENT="$ambient" \
    -x c -c -o "$scratch/guards.o" - <<'C'
#include "weather_protocols.h"
_Static_assert(WEATHER_PROTOCOL_LACROSSE_TX5U == EXPECT_OOK, "TX5U band");
_Static_assert(WEATHER_PROTOCOL_ACURITE_IRIS == EXPECT_OOK, "Iris band");
_Static_assert(WEATHER_PROTOCOL_AMBIENT_ARRAY == EXPECT_AMBIENT, "Ambient band");
C
done
echo 'Radio band guards and omitted decoder symbols passed'
