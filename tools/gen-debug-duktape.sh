#!/usr/bin/env bash
set -euo pipefail
repo=$(cd "$(dirname "$0")/.." && pwd)
scratch=$(mktemp -d)
trap 'rm -rf "$scratch"' EXIT
curl -fsSL https://duktape.org/duktape-2.7.0.tar.xz -o "$scratch/duktape.tar.xz"
printf '%s  %s\n' 90f8d2fa8b5567c6899830ddef2c03f3c27960b11aca222fa17aa7ac613c2890 "$scratch/duktape.tar.xz" | sha256sum -c -
tar -xf "$scratch/duktape.tar.xz" -C "$scratch"
mkdir -p "$scratch/generated"
docker run --rm --user "$(id -u):$(id -g)" -e HOME=/tmp -v "$scratch/duktape-2.7.0:/duk:ro" \
  -v "$scratch/generated:/out" python:2.7 sh -c \
  "pip install --user 'PyYAML==5.4.1' && python /duk/tools/configure.py --output-directory /out --rom-support -UDUK_USE_HSTRING_ARRIDX -DDUK_USE_ROM_STRINGS -DDUK_USE_ROM_OBJECTS -DDUK_USE_ROM_GLOBAL_INHERIT -DDUK_USE_INTERRUPT_COUNTER '-DDUK_USE_EXEC_TIMEOUT_CHECK=rainlog_duk_timeout'"
mkdir -p "$repo/components/debug_console/vendor"
cp "$scratch/generated/"* "$repo/components/debug_console/vendor/"
cp "$scratch/duktape-2.7.0/LICENSE.txt" "$scratch/duktape-2.7.0/AUTHORS.rst" "$repo/components/debug_console/vendor/"
