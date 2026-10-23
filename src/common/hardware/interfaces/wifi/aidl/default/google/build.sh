#!/usr/bin/env bash
# NDK build of AOSP wifi/aidl/default against frozen IWifi AIDL v2.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
_d="$HERE"
while [[ "$_d" != "/" && ! -f "$_d/build/hal-env.sh" ]]; do _d=$(dirname "$_d"); done
# shellcheck source=/dev/null
source "$_d/build/hal-env.sh"
HI="$ROOT/thirdparty/hardware-interfaces"
FROZEN="$HI/wifi/aidl/aidl_api/android.hardware.wifi/2"
COMMON_WIFI="$HI/wifi/common/aidl/aidl_api/android.hardware.wifi.common/1"
COMMON="$HI/common/aidl/aidl_api/android.hardware.common/2"
OUT="$ROOT/out/wifi-stub"
GEN="$ROOT/out/wifi-ndk-gen"
SHIM="${GKI_HAL_SHIM:-$ROOT/src/common/hardware/shim}"
CC="$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/aarch64-linux-android34-clang++"
HASH=$(cat "$FROZEN/.hash")
[[ -x "$CC" && -x "$AIDL" ]] || { echo 'missing NDK/aidl' >&2; exit 1; }

rm -rf "$GEN" "$OUT/obj" "$OUT/aidl-trim"
mkdir -p "$GEN/src" "$GEN/include" "$OUT/bin" "$OUT/obj" "$OUT/aidl-trim"

TRIM="$OUT/aidl-trim"
cp -a "$COMMON" "$TRIM/common"
cp -a "$COMMON_WIFI" "$TRIM/wifi-common"
cp -a "$FROZEN" "$TRIM/wifi"
mkdir -p "$TRIM/wifi-common/android/os"
cat > "$TRIM/wifi-common/android/os/PersistableBundle.aidl" <<'EOF'
package android.os;
@VintfStability
parcelable PersistableBundle {}
EOF

PACKAGES=("$TRIM/common" "$TRIM/wifi-common" "$TRIM/wifi")
INCLUDES=()
for package in "${PACKAGES[@]}"; do
  [[ -s "$package/.hash" ]] || { echo "missing $package" >&2; exit 1; }
  INCLUDES+=(-I "$package")
done

for package in "${PACKAGES[@]}"; do
  sources=()
  while IFS= read -r source; do sources+=("$source"); done < <(find "$package" -name '*.aidl' | sort)
  case "$package" in
    */wifi-common) ver=$(basename "$COMMON_WIFI"); hash=$(cat "$COMMON_WIFI/.hash") ;;
    */wifi) ver=$(basename "$FROZEN"); hash=$(cat "$FROZEN/.hash") ;;
    */common) ver=$(basename "$COMMON"); hash=$(cat "$COMMON/.hash") ;;
    *) echo "unknown package $package" >&2; exit 1 ;;
  esac
  "$AIDL" --lang=ndk --structured --stability=vintf --min_sdk_version=34 \
    --version="$ver" --hash="$hash" \
    -o "$GEN/src" -h "$GEN/include" "${INCLUDES[@]}" "${sources[@]}"
done

objects=()
compile_one() {
  local src=$1
  local obj
  obj="$OUT/obj/$(echo "$src" | tr '/.' '__').o"
  mkdir -p "$(dirname "$obj")"
  echo "  CC $(basename "$src")"
  "$CC" -c "$src" -o "$obj" -std=c++20 -O2 -fPIC \
    -DBINDER_STABILITY_SUPPORT -DLOG_TAG='"android.hardware.wifi-service"' \
    -I"$HERE" -I"$HERE/include" -I"$SHIM" \
    -I"$GEN/include" -I"$ANDROID_HEADERS" \
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-macro-redefined -Wno-gcc-compat -Wno-inconsistent-missing-override
  objects+=("$obj")
}

while IFS= read -r src; do compile_one "$src"; done < <(find "$GEN/src" -name '*.cpp' | sort)
while IFS= read -r src; do
  case "$src" in
    */include/wifi_system/interface_tool.cpp) continue ;;
  esac
  compile_one "$src"
done < <(find "$HERE" -name '*.cpp' | sort)
compile_one "$SHIM/cutils/properties.cpp"

"$CC" -o "$OUT/bin/android.hardware.wifi-service" "${objects[@]}" \
  -static-libstdc++ -L"$GSI_LIBS" -lbinder_ndk -llog -ldl -Wl,--allow-shlib-undefined
echo "Built $OUT/bin/android.hardware.wifi-service (AOSP wifi/aidl/default, IWifi v2 hash=$HASH)"
