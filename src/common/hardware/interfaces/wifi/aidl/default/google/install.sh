#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
_d="$HERE"
while [[ "$_d" != "/" && ! -f "$_d/build/hal-env.sh" ]]; do _d=$(dirname "$_d"); done
# shellcheck source=/dev/null
source "$_d/build/hal-env.sh"
mkdir -p "$STUB/bin/hw" "$STUB/bin" "$STUB/etc/init" "$STUB/etc/vintf/manifest"
install -m 0755 "$ROOT/out/wifi-stub/bin/android.hardware.wifi-service" \
  "$STUB/bin/hw/android.hardware.wifi-service"
install -m 0755 "$HERE/qemu-wifi-fake-scan.sh" "$STUB/bin/qemu-wifi-fake-scan.sh"
install -m 0644 "$HERE/../init/android.hardware.wifi-service.rc" "$STUB/etc/init/"
install -m 0644 "$HERE/../init/qemu-wifi-fake-scan.rc" "$STUB/etc/init/"
install -m 0644 "$HERE/../vintf/android.hardware.wifi-service.xml" \
  "$STUB/etc/vintf/manifest/"
FC="$STUB/etc/selinux/vendor_file_contexts"
if [[ -f "$FC" ]] && ! grep -q 'android\.hardware\.wifi-service' "$FC"; then
  printf '/vendor/bin/hw/android\\.hardware\\.wifi-service    u:object_r:hal_keymint_system_exec:s0\n' \
    >> "$FC"
fi
if [[ -f "$FC" ]] && ! grep -q 'qemu-wifi-fake-scan' "$FC"; then
  printf '/vendor/bin/qemu-wifi-fake-scan\\.sh    u:object_r:hal_keymint_system_exec:s0\n' \
    >> "$FC"
fi
