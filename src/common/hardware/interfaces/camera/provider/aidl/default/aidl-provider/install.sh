#!/usr/bin/env bash
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
_d="$HERE"
while [[ "$_d" != "/" && ! -f "$_d/build/hal-env.sh" ]]; do _d=$(dirname "$_d"); done
# shellcheck source=/dev/null
source "$_d/build/hal-env.sh"
DEST="$AOSP/qemu/vendor"
mkdir -p "$DEST/bin/hw" "$DEST/etc/init" "$DEST/etc/vintf/manifest"
install -m 0755 "$ROOT/out/camera-aidl-provider/bin/android.hardware.camera.provider-service" \
  "$DEST/bin/hw/android.hardware.camera.provider-service"
install -m 0644 "$HERE/android.hardware.camera.provider-service.rc" \
  "$DEST/etc/init/android.hardware.camera.provider-service.rc"
install -m 0644 "$HERE/android.hardware.camera.provider-service.xml" \
  "$DEST/etc/vintf/manifest/android.hardware.camera.provider-service.xml"
# HAL1 camera.default.so was a homemade stub; CVD AIDL provider replaces it.
rm -f "$DEST/lib64/hw/camera.default.so"
FC="$DEST/etc/selinux/vendor_file_contexts"
if [[ -f "$FC" ]] && ! grep -q 'android\.hardware\.camera\.provider-service' "$FC"; then
  printf '/vendor/bin/hw/android\\.hardware\\.camera\\.provider-service    u:object_r:hal_keymint_system_exec:s0\n' \
    >> "$FC"
fi
