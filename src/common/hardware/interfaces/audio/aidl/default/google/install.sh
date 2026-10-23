#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
_d="$HERE"
while [[ "$_d" != "/" && ! -f "$_d/build/hal-env.sh" ]]; do _d=$(dirname "$_d"); done
# shellcheck source=/dev/null
source "$_d/build/hal-env.sh"
mkdir -p "$STUB/bin/hw" "$STUB/etc/init" "$STUB/etc/vintf/manifest"
install -m 0755 "$ROOT/out/audio-stub/bin/android.hardware.audio-service" \
  "$STUB/bin/hw/android.hardware.audio-service"
install -m 0644 "$HERE/../init/android.hardware.audio-service.rc" "$STUB/etc/init/"
install -m 0644 "$HERE/../vintf/android.hardware.audio-service.xml" \
  "$STUB/etc/vintf/manifest/"
FC="$STUB/etc/selinux/vendor_file_contexts"
if [[ -f "$FC" ]] && ! grep -q 'android\.hardware\.audio-service' "$FC"; then
  printf '/vendor/bin/hw/android\\.hardware\\.audio-service    u:object_r:hal_keymint_system_exec:s0\n' \
    >> "$FC"
fi
