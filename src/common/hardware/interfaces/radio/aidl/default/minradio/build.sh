#!/usr/bin/env bash
# NDK build of AOSP radio/aidl/minradio against GSI FCM Radio AIDL v3.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
_d="$HERE"
while [[ "$_d" != "/" && ! -f "$_d/build/hal-env.sh" ]]; do _d=$(dirname "$_d"); done
# shellcheck source=/dev/null
source "$_d/build/hal-env.sh"
HI="$ROOT/thirdparty/hardware-interfaces"
OUT="$ROOT/out/radio-stub"
GEN="$ROOT/out/radio-ndk-gen"
SHIM="${GKI_HAL_SHIM:-$ROOT/src/common/hardware/shim}"
CC="$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/aarch64-linux-android34-clang++"
[[ -x "$CC" && -x "$AIDL" ]] || { echo 'missing NDK/aidl' >&2; exit 1; }

PACKAGES=(
  "$HI/radio/aidl/aidl_api/android.hardware.radio/3"
  "$HI/radio/aidl/aidl_api/android.hardware.radio.config/3"
  "$HI/radio/aidl/aidl_api/android.hardware.radio.modem/3"
  "$HI/radio/aidl/aidl_api/android.hardware.radio.network/3"
  "$HI/radio/aidl/aidl_api/android.hardware.radio.sim/3"
  "$HI/radio/aidl/aidl_api/android.hardware.radio.data/3"
  "$HI/radio/aidl/aidl_api/android.hardware.radio.messaging/3"
  "$HI/radio/aidl/aidl_api/android.hardware.radio.voice/3"
)

rm -rf "$GEN" "$OUT/obj"
mkdir -p "$GEN/src" "$GEN/include" "$OUT/bin" "$OUT/obj"

INCLUDES=()
for package in "${PACKAGES[@]}"; do
  [[ -s "$package/.hash" ]] || { echo "missing $package" >&2; exit 1; }
  INCLUDES+=(-I "$package")
done
for package in "${PACKAGES[@]}"; do
  sources=()
  while IFS= read -r source; do sources+=("$source"); done < <(find "$package" -name '*.aidl' | sort)
  "$AIDL" --lang=ndk --structured --stability=vintf --min_sdk_version=34 \
    --version=3 --hash="$(cat "$package/.hash")" \
    -o "$GEN/src" -h "$GEN/include" "${INCLUDES[@]}" "${sources[@]}"
done

python3 "$HERE/generate-stubs.py" "$GEN/include" "$GEN/include/radio_stubs.h"

# Frozen Radio AIDL v3 RadioConst has MAX_RILDS / CARD_MAX_APPS / P2_CONSTANT_NO_P2
# only. VALUE_UNAVAILABLE* arrived in v4; minradio still uses those sentinels.
python3 - "$GEN/include/aidl/android/hardware/radio/RadioConst.h" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1])
text = p.read_text()
needle = "enum : int32_t { P2_CONSTANT_NO_P2 = -1 };"
insert = (
    needle
    + "\n  enum : int32_t { VALUE_UNAVAILABLE = 0x7FFFFFFF };"
    + "\n  enum : int64_t { VALUE_UNAVAILABLE_LONG = 0x7FFFFFFFFFFFFFFFLL };"
)
if needle not in text:
    raise SystemExit("RadioConst.h missing P2_CONSTANT_NO_P2")
if "VALUE_UNAVAILABLE =" not in text:
    p.write_text(text.replace(needle, insert, 1))
PY

objects=()
compile_one() {
  local src=$1
  local obj
  obj="$OUT/obj/$(echo "$src" | tr '/.' '__').o"
  mkdir -p "$(dirname "$obj")"
  echo "  CC $(basename "$src")"
  "$CC" -c "$src" -o "$obj" -std=c++20 -O2 -fPIC \
    -DBINDER_STABILITY_SUPPORT -DLOG_TAG='"minradio"' \
    -I"$HERE" -I"$HERE/libminradio/include" -I"$HERE/impl" \
    -I"$GEN/include" -I"$SHIM" -I"$ANDROID_HEADERS" \
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-macro-redefined -Wno-gcc-compat
  objects+=("$obj")
}
while IFS= read -r src; do compile_one "$src"; done < <(find "$GEN/src" -name '*.cpp' | sort)
while IFS= read -r src; do compile_one "$src"; done < <(find "$HERE/libminradio" -name '*.cpp' | sort)
for src in "$HERE"/impl/*.cpp "$HERE"/service.cpp; do
  compile_one "$src"
done
compile_one "$SHIM/cutils/properties.cpp"

"$CC" -o "$OUT/bin/android.hardware.radio-service" "${objects[@]}" \
  -static-libstdc++ -L"$GSI_LIBS" -lbinder_ndk -llog -Wl,--allow-shlib-undefined
echo "Built $OUT/bin/android.hardware.radio-service (AOSP minradio, Radio AIDL v3)"
