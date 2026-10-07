#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
bin_dir=$(mktemp -d)
trap 'rm -rf "$bin_dir"' EXIT
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined \
  -I components/debug_console/include tests/console_line_test.c -o "$bin_dir/console-line"
"$bin_dir/console-line"
echo 'Console line parsing checks passed'
