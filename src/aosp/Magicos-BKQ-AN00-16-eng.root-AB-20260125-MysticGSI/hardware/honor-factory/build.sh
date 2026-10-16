#!/usr/bin/env bash
# Build the placeholder Honor factory HAL (see service.cpp) and install it
# with its init rc and VINTF fragment into qemu/vendor.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
VARIANT=$(basename "$(cd "$HERE/../.." && pwd)")
# shellcheck source=scripts/env.sh
source "$ROOT/scripts/env.sh"
GSI_LIBS="$ROOT/src/aosp/$VARIANT/prebuilts/gsi-lib64"
CC=$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/aarch64-linux-android34-clang++
VENDOR="$HERE/../../qemu/vendor"
OUT="$ROOT/out/honor-factory"
mkdir -p "$OUT" "$VENDOR/bin/hw" "$VENDOR/etc/init" "$VENDOR/etc/vintf/manifest"
"$CC" -std=c++20 -O2 -Wall -I"$ANDROID_HEADERS" \
  "$HERE/service.cpp" -o "$OUT/vendor.honor.hardware.hwfactoryinterface-service" \
  -L"$GSI_LIBS" -lbinder_ndk -llog -static-libstdc++ -Wl,--allow-shlib-undefined
install -m 0755 "$OUT/vendor.honor.hardware.hwfactoryinterface-service" "$VENDOR/bin/hw/"
install -m 0644 "$HERE/init/vendor.honor.hardware.hwfactoryinterface-service.rc" "$VENDOR/etc/init/"
install -m 0644 "$HERE/vintf/vendor.honor.hardware.hwfactoryinterface-service.xml" \
  "$VENDOR/etc/vintf/manifest/"
echo "installed vendor.honor.hardware.hwfactoryinterface-service into $VENDOR"
