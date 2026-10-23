#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
_d="$HERE"
while [[ "$_d" != "/" && ! -f "$_d/build/hal-env.sh" ]]; do _d=$(dirname "$_d"); done
# shellcheck source=/dev/null
source "$_d/build/hal-env.sh"
install -m 0755 "$ROOT/out/keymint-soft/bin/android.hardware.security.keymint-service" \
    "$AOSP/qemu/vendor/bin/hw/android.hardware.security.keymint-service"
