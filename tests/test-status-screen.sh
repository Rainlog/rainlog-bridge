#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
for font in BOARD_FONT_4X6; do
  # Compile the actual drawing code with mock data providers. Strip include
  # directives only; the firmware builds check its real header dependencies.
  python3 - "$scratch/status_under_test.c" <<'PY'
import re, sys
from pathlib import Path
text = Path('main/ui/ui.c').read_text()
text = text[text.index('#if BOARD_DISPLAY_SSD1306'):text.index('\n#endif\n\ntypedef struct')+7]
text = re.sub(r'^#include .*$', '', text, flags=re.M)
text = '#include "oled_logo.h"\n'+text
Path(sys.argv[1]).write_text(text)
PY
  cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
    -DCONFIG_IDF_TARGET_ESP32=1 -D "BOARD_DISPLAY_FONT=$font" -DRAINLOG_RADIO=1 -DRAINLOG_RADIO_MHZ=433 \
    -I "$scratch" -I tests/mocks -I main -I main/ui \
    tests/status_screen_test.c -o "$scratch/test"
  if [[ "$font" == BOARD_FONT_4X6 ]]; then
    "$scratch/test" /tmp/rainlog-oled-dense.pgm
  else
    "$scratch/test"
  fi
done
