#!/usr/bin/env bash
# Build the ColorOS charger HAL (vendor.oplus.hardware.charger.ICharger V10)
# and install it with its init rc and VINTF fragment into qemu/vendor.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
VARIANT=$(basename "$(cd "$HERE/../.." && pwd)")
# shellcheck source=scripts/env.sh
source "$ROOT/scripts/env.sh"
GSI_LIBS="$ROOT/src/aosp/$VARIANT/prebuilts/gsi-lib64"
[[ -f "$GSI_LIBS/libbinder_ndk.so" ]] || GSI_LIBS="$ROOT/src/aosp/aosp_arm64-BP4A.251205.006/prebuilts/gsi-lib64"
CC=$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/aarch64-linux-android34-clang++
VENDOR="$HERE/../../qemu/vendor"
OUT="$ROOT/out/oplus-charger"
mkdir -p "$OUT" "$VENDOR/bin/hw" "$VENDOR/etc/init" "$VENDOR/etc/vintf/manifest"
"$CC" -std=c++20 -O2 -Wall -I"$ANDROID_HEADERS" -I"$HERE" \
  "$HERE/service.cpp" -o "$OUT/vendor.oplus.hardware.charger-service" \
  -L"$GSI_LIBS" -lbinder_ndk -llog -static-libstdc++ -Wl,--allow-shlib-undefined
install -m 0755 "$OUT/vendor.oplus.hardware.charger-service" "$VENDOR/bin/hw/"
install -m 0644 "$HERE/init/vendor.oplus.hardware.charger-service.rc" "$VENDOR/etc/init/"
install -m 0644 "$HERE/vintf/vendor.oplus.hardware.charger-service.xml" "$VENDOR/etc/vintf/manifest/"
echo "installed vendor.oplus.hardware.charger-service into $VENDOR"
