#!/usr/bin/env bash
# KMS composer + stub IAllocator/mapper (VirGL GBM or gfxstream virtgpu).
set -euo pipefail
LOCAL_PATH=$(cd "$(dirname "$0")" && pwd)
_d="$LOCAL_PATH"
while [[ "$_d" != "/" && ! -f "$_d/build/hal-env.sh" ]]; do _d=$(dirname "$_d"); done
# shellcheck source=/dev/null
source "$_d/build/hal-env.sh"
GKI_VENDOR="${GKI_VENDOR:-$ROOT/src/aosp/$VARIANT/qemu/vendor}"

mkdir -p "$GKI_VENDOR/bin/hw" "$GKI_VENDOR/lib64/hw" \
  "$GKI_VENDOR/etc/init" "$GKI_VENDOR/etc/vintf/manifest"

bash "$LOCAL_PATH/stub/build-stub.sh"
bash "$LOCAL_PATH/stub/build-mapper.sh"
install -m 0755 "$ROOT/out/graphics-stub/bin/android.hardware.graphics-service" \
  "$GKI_VENDOR/bin/hw/"
install -m 0755 "$ROOT/out/graphics-stub/lib64/hw/mapper.stub.so" "$GKI_VENDOR/lib64/hw/"
install -m 0644 "$ROOT/out/graphics-stub/lib64/hw/mapper.stub.so" \
  "$GKI_VENDOR/lib64/mapper.stub.so"
install -m 0644 "$LOCAL_PATH/init/android.hardware.graphics-service.rc" \
  "$GKI_VENDOR/etc/init/"
install -m 0644 "$LOCAL_PATH/vintf/manifest.graphics.xml" \
  "$GKI_VENDOR/etc/vintf/manifest/"
# Drop any leftover cros_gralloc minigbm service from older installs.
rm -f \
  "$GKI_VENDOR/bin/hw/android.hardware.graphics.allocator-service.minigbm" \
  "$GKI_VENDOR/lib64/hw/mapper.minigbm.so" \
  "$GKI_VENDOR/lib64/mapper.minigbm.so" \
  "$GKI_VENDOR/etc/init/android.hardware.graphics.allocator-service.minigbm.rc" \
  "$GKI_VENDOR/etc/vintf/manifest/mapper.minigbm.xml" \
  "$GKI_VENDOR/etc/vintf/manifest/mapper.minigbm.xml.disabled" \
  "$GKI_VENDOR/etc/vintf/manifest/manifest_allocator.minigbm.xml" \
  "$GKI_VENDOR/etc/vintf/manifest/manifest_allocator.xml"
bash "$LOCAL_PATH/drm_hwcomposer/build.sh"
