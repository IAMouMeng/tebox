#!/usr/bin/env bash
# Soft NDK WiFi HAL (IWifi AIDL v2) with a simulated chip + STA iface.
# Scan list is injected at runtime via cmd wifi add-fake-scan (no kernel/nl80211).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../../.." && pwd)
source "$ROOT/scripts/env.sh"
HI="$ROOT/thirdparty/hardware-interfaces"
FROZEN="$HI/wifi/aidl/aidl_api/android.hardware.wifi/2"
OUT="$ROOT/out/wifi-stub"
GEN="$ROOT/out/wifi-ndk-gen"
TRIM="$ROOT/out/wifi-aidl-trim"
CC="$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/aarch64-linux-android34-clang++"
HASH=$(cat "$FROZEN/.hash")
[[ -x "$CC" && -x "$AIDL" ]] || { echo 'missing NDK/aidl' >&2; exit 1; }

rm -rf "$GEN" "$OUT/obj" "$TRIM"
mkdir -p "$GEN/src" "$GEN/include" "$OUT/bin" "$OUT/obj"
mkdir -p "$TRIM/android/hardware/wifi"

cp "$FROZEN/android/hardware/wifi/IWifi.aidl" "$TRIM/android/hardware/wifi/"
cp "$FROZEN/android/hardware/wifi/IWifiEventCallback.aidl" "$TRIM/android/hardware/wifi/"
cp "$FROZEN/android/hardware/wifi/WifiStatusCode.aidl" "$TRIM/android/hardware/wifi/"
cp "$FROZEN/android/hardware/wifi/IfaceConcurrencyType.aidl" "$TRIM/android/hardware/wifi/"

cat > "$TRIM/android/hardware/wifi/IWifiStaIfaceEventCallback.aidl" <<'EOF'
package android.hardware.wifi;
@VintfStability
interface IWifiStaIfaceEventCallback {}
EOF

cat > "$TRIM/android/hardware/wifi/IWifiStaIface.aidl" <<'EOF'
package android.hardware.wifi;
@VintfStability
interface IWifiStaIface {
  String getName();
  int getFeatureSet();
  byte[6] getFactoryMacAddress();
  void setMacAddress(in byte[6] mac);
  void setScanMode(in boolean enable);
  void registerEventCallback(in android.hardware.wifi.IWifiStaIfaceEventCallback callback);
}
EOF

cat > "$TRIM/android/hardware/wifi/IWifiChipEventCallback.aidl" <<'EOF'
package android.hardware.wifi;
@VintfStability
interface IWifiChipEventCallback {
  oneway void onChipReconfigureFailure(in android.hardware.wifi.WifiStatusCode status);
  oneway void onChipReconfigured(in int modeId);
}
EOF

cat > "$TRIM/android/hardware/wifi/IWifiChip.aidl" <<'EOF'
package android.hardware.wifi;
@VintfStability
interface IWifiChip {
  void configureChip(in int modeId);
  @PropagateAllowBlocking android.hardware.wifi.IWifiStaIface createStaIface();
  android.hardware.wifi.IWifiChip.ChipMode[] getAvailableModes();
  int getFeatureSet();
  int getId();
  int getMode();
  void registerEventCallback(in android.hardware.wifi.IWifiChipEventCallback callback);
  void removeStaIface(in String ifname);
  void setCountryCode(in byte[2] code);

  @VintfStability
  parcelable ChipConcurrencyCombinationLimit {
    android.hardware.wifi.IfaceConcurrencyType[] types;
    int maxIfaces;
  }
  @VintfStability
  parcelable ChipConcurrencyCombination {
    android.hardware.wifi.IWifiChip.ChipConcurrencyCombinationLimit[] limits;
  }
  @VintfStability
  parcelable ChipMode {
    int id;
    android.hardware.wifi.IWifiChip.ChipConcurrencyCombination[] availableCombinations;
  }
}
EOF
echo "$HASH" > "$TRIM/.hash"

sources=()
while IFS= read -r source; do sources+=("$source"); done < <(find "$TRIM" -name '*.aidl' | sort)
"$AIDL" --lang=ndk --structured --stability=vintf --min_sdk_version=34 \
  --version=2 --hash="$HASH" \
  -o "$GEN/src" -h "$GEN/include" -I "$TRIM" "${sources[@]}"

objects=()
while IFS= read -r src; do
  obj="$OUT/obj/$(echo "${src#"$GEN/src/"}" | tr / _).o"
  mkdir -p "$(dirname "$obj")"
  "$CC" -c "$src" -o "$obj" -std=c++20 -O2 -fPIC \
    -DBINDER_STABILITY_SUPPORT -DLOG_TAG='"qemu-wifi"' \
    -I"$GEN/include" -I"$ANDROID_HEADERS" -Wno-unused-parameter
  objects+=("$obj")
done < <(find "$GEN/src" -name '*.cpp' | sort)

"$CC" -c "$HERE/service.cpp" -o "$OUT/obj/service.o" -std=c++20 -O2 -fPIC \
  -DBINDER_STABILITY_SUPPORT -DLOG_TAG='"qemu-wifi"' \
  -I"$GEN/include" -I"$ANDROID_HEADERS" -Wno-unused-parameter

"$CC" -o "$OUT/bin/android.hardware.wifi-service" "$OUT/obj/service.o" "${objects[@]}" \
  -static-libstdc++ -L"$GSI_LIBS" -lbinder_ndk -llog -Wl,--allow-shlib-undefined
echo "Built $OUT/bin/android.hardware.wifi-service (IWifi hash=$HASH)"
