#!/usr/bin/env bash
# NDK launcher of AOSP keymint/aidl/default (AndroidKeyMintDevice in GSI
# libkeymint_fake_latest). CVD remote KeyMint is vsock to a host daemon.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
_d="$HERE"
while [[ "$_d" != "/" && ! -f "$_d/build/hal-env.sh" ]]; do _d=$(dirname "$_d"); done
# shellcheck source=/dev/null
source "$_d/build/hal-env.sh"
OUT="$ROOT/out/keymint-soft"
GEN="$OUT/aidl"
LIBS="$GSI_LIBS"
HI="$ROOT/thirdparty/hardware-interfaces/security"
SHIM="${GKI_HAL_SHIM:-$ROOT/src/common/hardware/shim}"
SOFT="$HERE/../soft"
CC="$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/aarch64-linux-android34-clang++"
mkdir -p "$GEN/include" "$GEN/src" "$LIBS" "$OUT/bin"
bash "$ROOT/build/soong/extract-gsi-libs.sh" "$VARIANT"
KM="$HI/keymint/aidl/aidl_api/android.hardware.security.keymint/4"
SC="$HI/secureclock/aidl/aidl_api/android.hardware.security.secureclock/1"
SS="$HI/sharedsecret/aidl/aidl_api/android.hardware.security.sharedsecret/1"
HASH=$(cat "$KM/.hash")
[[ -x "$CC" && -x "$AIDL" ]] || { echo 'missing NDK/aidl' >&2; exit 1; }
for spec in "$KM:4" "$SC:1" "$SS:1"; do
    api=${spec%:*}
    version=${spec##*:}
    files=()
    while IFS= read -r file; do files+=("$file"); done < <(find "$api" -name '*.aidl' | sort)
    "$AIDL" --lang=ndk --structured --stability=vintf --version="$version" \
        --hash="$(cat "$api/.hash")" -I "$KM" -I "$SC" -I "$SS" \
        -h "$GEN/include" -o "$GEN/src" "${files[@]}"
done
# Platform C++ interfaces: use the platform's __1 ABI and libc++, not NDK __ndk1.
"$CC" -std=c++20 -O2 -fno-rtti -D__ndk1=__1 -DBINDER_STABILITY_SUPPORT \
    -I"$SOFT/include" -I"$SHIM" -I"$GEN/include" -I"$ANDROID_HEADERS" \
    -I"$ROOT/thirdparty/system-keymaster/ng/include" \
    -I"$ROOT/thirdparty/system-keymaster/include" \
    "$HERE/service.cpp" "$SOFT/persistent_storage.cpp" "$SOFT/root_of_trust.cpp" \
    "$ROOT/thirdparty/system-keymaster/contexts/soft_keymaster_logger.cpp" \
    -nostdlib++ -L"$LIBS" -Wl,-rpath,/system/lib64 -Wl,--export-dynamic \
    -lkeymint_fake_latest -lkeymaster_portable -lbinder_ndk -llog -lc++ \
    -l:android.hardware.security.keymint-V4-ndk.so \
    -l:android.hardware.security.secureclock-V1-ndk.so \
    -l:android.hardware.security.sharedsecret-V1-ndk.so \
    -Wl,--allow-shlib-undefined -o "$OUT/bin/android.hardware.security.keymint-service"
echo "Built $OUT/bin/android.hardware.security.keymint-service (AOSP keymint/aidl/default, IKeyMint v4 hash=$HASH)"
