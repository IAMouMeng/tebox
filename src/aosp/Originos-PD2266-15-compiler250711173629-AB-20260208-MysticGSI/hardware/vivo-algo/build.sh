#!/usr/bin/env bash
# Build the placeholder vivo algorithm HAL (see service.cpp) and install it
# with its init rc into qemu/vendor.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
VARIANT=$(basename "$(cd "$HERE/../.." && pwd)")
# shellcheck source=scripts/env.sh
source "$ROOT/scripts/env.sh"
GSI_LIBS="$ROOT/src/aosp/$VARIANT/prebuilts/gsi-lib64"
CC=$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/aarch64-linux-android34-clang++
VENDOR="$HERE/../../qemu/vendor"
OUT="$ROOT/out/vivo-algo"
mkdir -p "$OUT" "$VENDOR/bin/hw" "$VENDOR/etc/init"
"$CC" -std=c++20 -O2 -Wall -I"$ANDROID_HEADERS" \
  "$HERE/service.cpp" -o "$OUT/vendor.vivo.hardware.vivoalgoservice-service" \
  -L"$GSI_LIBS" -lbinder_ndk -llog -static-libstdc++ -Wl,--allow-shlib-undefined
install -m 0755 "$OUT/vendor.vivo.hardware.vivoalgoservice-service" "$VENDOR/bin/hw/"
install -m 0644 "$HERE/init/vendor.vivo.hardware.vivoalgoservice-service.rc" "$VENDOR/etc/init/"
echo "installed vendor.vivo.hardware.vivoalgoservice-service into $VENDOR"
