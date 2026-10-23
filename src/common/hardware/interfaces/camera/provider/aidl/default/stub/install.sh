#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
_d="$HERE"
while [[ "$_d" != "/" && ! -f "$_d/build/hal-env.sh" ]]; do _d=$(dirname "$_d"); done
# shellcheck source=/dev/null
source "$_d/build/hal-env.sh"
DEST="$AOSP/qemu/vendor/lib64/hw"
mkdir -p "$DEST"
install -m 0755 "$ROOT/out/camera-stub/lib64/camera.default.so" "$DEST/camera.default.so"
