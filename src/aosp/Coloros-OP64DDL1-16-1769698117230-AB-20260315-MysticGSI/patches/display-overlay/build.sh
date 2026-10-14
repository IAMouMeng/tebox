#!/usr/bin/env bash
# Build the display overlay. Same resources as ColorOS's overlay except
# rounded_corner_radius and the display cutout. Two copies, both bind-mounted
# over the GSI's (system.img untouched):
#   qemu/vendor/etc/tebox/display-overlay.apk  -> /system/product/overlay/...
#     by init.ranchu.rc (an overlayfs; resources load from it)
#   qemu/init/system-overlay/my_product/overlay -> /my_product/overlay/...
#     by the initramfs (the path PackageManager lists)
# plus oplus.product.display_features.xml without origin_roundcorner_support,
# which otherwise makes ColorOS SystemUI draw its own corner masks.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
SDK="$ROOT/toolchains/android-sdk"
BT="$SDK/build-tools/36.1.0"
JAR="$SDK/platforms/android-36/android.jar"
OUT="$ROOT/out/display-overlay"
NAME=android_framework_res_overlay.display.product.25821.apk
VENDOR="$HERE/../../qemu/vendor/etc/tebox"
DEST="$HERE/../../qemu/init/system-overlay/my_product/overlay"
rm -rf "$OUT" && mkdir -p "$OUT" "$DEST" "$VENDOR"
"$BT/aapt2" compile --dir "$HERE/res" -o "$OUT/res.zip"
"$BT/aapt2" link -I "$JAR" --manifest "$HERE/AndroidManifest.xml" \
  --min-sdk-version 36 --target-sdk-version 36 -o "$OUT/unsigned.apk" "$OUT/res.zip"
"$BT/zipalign" -f -p 4 "$OUT/unsigned.apk" "$OUT/aligned.apk"
# Preinstalled overlays need a valid signature, not a particular key.
keytool -genkeypair -keystore "$OUT/key.jks" -storepass tebox123 -keypass tebox123 \
  -alias tebox -keyalg RSA -keysize 2048 -validity 10000 -dname CN=tebox >/dev/null 2>&1
"$BT/apksigner" sign --ks "$OUT/key.jks" --ks-pass pass:tebox123 --ks-key-alias tebox \
  --out "$DEST/$NAME" "$OUT/aligned.apk"
rm -f "$DEST/$NAME.idsig"
install -m 0644 "$DEST/$NAME" "$VENDOR/display-overlay.apk"
PERM="$HERE/../../qemu/init/system-overlay/my_product/etc/permissions"
mkdir -p "$PERM"
install -m 0644 "$HERE/oplus.product.display_features.xml" "$PERM/"
echo "installed $DEST/$NAME and $VENDOR/display-overlay.apk"
