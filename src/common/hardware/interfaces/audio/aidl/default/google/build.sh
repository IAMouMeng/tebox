#!/usr/bin/env bash
# NDK build of AOSP audio/aidl/default ModuleStub + Configuration (skip xsdc).
# DriverStubImpl writes/reads QEMU virtio-serial tebox.audio_out / tebox.audio_in.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
_d="$HERE"
while [[ "$_d" != "/" && ! -f "$_d/build/hal-env.sh" ]]; do _d=$(dirname "$_d"); done
# shellcheck source=/dev/null
source "$_d/build/hal-env.sh"
GEN="$ROOT/out/audio-ndk-gen"
OUT="$ROOT/out/audio-stub"
HI="$ROOT/thirdparty/hardware-interfaces"
MEDIA="$ROOT/thirdparty/audio-deps/media/aidl_api/android.media.audio.common.types/2"
FMQ="$ROOT/thirdparty/reference/aosp-audio/libfmq"
SHIM="${GKI_HAL_SHIM:-$ROOT/src/common/hardware/shim}"
CC="$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/aarch64-linux-android34-clang++"
PACKAGES=(
  "$HI/common/aidl/aidl_api/android.hardware.common/2"
  "$HI/common/fmq/aidl/aidl_api/android.hardware.common.fmq/1"
  "$MEDIA"
  "$HI/audio/aidl/aidl_api/android.hardware.audio.common/2"
  "$HI/audio/aidl/aidl_api/android.hardware.audio.core.sounddose/1"
  "$HI/audio/aidl/aidl_api/android.hardware.audio.effect/1"
  "$HI/audio/aidl/aidl_api/android.hardware.audio.core/1"
)
INCLUDES=()
for package in "${PACKAGES[@]}"; do
  [[ -s "$package/.hash" ]] || { echo "missing frozen AIDL: $package" >&2; exit 1; }
  INCLUDES+=(-I "$package")
done
[[ -x "$CC" && -x "$AIDL" ]] || { echo 'missing NDK compiler or aidl' >&2; exit 1; }

rm -rf "$GEN" "$OUT/obj"
mkdir -p "$GEN/src" "$GEN/include" "$OUT/bin" "$OUT/obj"
for package in "${PACKAGES[@]}"; do
  sources=()
  while IFS= read -r source; do sources+=("$source"); done < <(find "$package" -name '*.aidl' | sort)
  "$AIDL" --lang=ndk --structured --stability=vintf --min_sdk_version=34 \
    --version="$(basename "$package")" --hash="$(cat "$package/.hash")" \
    -o "$GEN/src" -h "$GEN/include" "${INCLUDES[@]}" "${sources[@]}"
done
if [[ -x "$HERE/../stub/generate-defaults.py" ]]; then
  python3 "$HERE/../stub/generate-defaults.py" "$GEN/include" || true
fi

objects=()
compile_one() {
  local src=$1
  local obj
  obj="$OUT/obj/$(echo "$src" | tr '/.' '__').o"
  mkdir -p "$(dirname "$obj")"
  echo "  CC $(basename "$src")"
  "$CC" -c "$src" -o "$obj" -std=c++20 -O2 -fPIC \
    -DBINDER_STABILITY_SUPPORT -DLOG_TAG='"AHAL_Main"' \
    -I"$HERE" -I"$HERE/include" -I"$HERE/stub" -I"$SHIM" \
    -I"$GEN/include" -I"$ANDROID_HEADERS" \
    -I"$FMQ/include" -I"$FMQ/base" \
    -I"$ROOT/thirdparty/reference/audio-smoke-headers" \
    -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable \
    -Wno-macro-redefined -Wno-gcc-compat -Wno-inconsistent-missing-override
  objects+=("$obj")
}

while IFS= read -r src; do compile_one "$src"; done < <(find "$GEN/src" -name '*.cpp' | sort)
for src in "$HERE"/Module.cpp "$HERE"/Stream.cpp "$HERE"/Configuration.cpp \
           "$HERE"/Config.cpp "$HERE"/Factory.cpp "$HERE"/Bluetooth.cpp \
           "$HERE"/Telephony.cpp "$HERE"/SoundDose.cpp \
           "$HERE"/common/StreamWorker.cpp \
           "$HERE"/stub/ModuleStub.cpp "$HERE"/stub/StreamStub.cpp \
           "$HERE"/stub/DriverStubImpl.cpp "$HERE"/stub/StreamMmapStub.cpp \
           "$HERE"/stub/StreamOffloadStub.cpp "$HERE"/stub/ApeHeader.cpp \
           "$HERE"/main.cpp; do
  compile_one "$src"
done
compile_one "$SHIM/cutils/properties.cpp"
compile_one "$SHIM/cutils/error_write_log.cpp"
for src in "$FMQ/EventFlag.cpp" "$FMQ/FmqInternal.cpp"; do
  compile_one "$src"
done

"$CC" -o "$OUT/bin/android.hardware.audio-service" "${objects[@]}" \
  -static-libstdc++ -L"$GSI_LIBS" -lbinder_ndk -llog -lcutils -Wl,--allow-shlib-undefined
echo "Built $OUT/bin/android.hardware.audio-service (AOSP ModuleStub + virtio PCM)"
