#!/usr/bin/env bash
# Build set-wallpaper.dex into the vendor tree (run by init.ranchu.rc once).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
SDK="$ROOT/toolchains/android-sdk"
JAR="$SDK/platforms/android-36/android.jar"
OUT="$ROOT/out/coloros-setup"
DEST="$HERE/../../qemu/vendor/etc/tebox"
rm -rf "$OUT" && mkdir -p "$OUT/classes" "$OUT/dex" "$DEST"
javac -source 8 -target 8 -Xlint:-options -nowarn -classpath "$JAR" -d "$OUT/classes" "$HERE/SetWallpaper.java"
"$SDK/build-tools/36.1.0/d8" --min-api 34 --lib "$JAR" --output "$OUT/dex" "$OUT/classes/SetWallpaper.class"
install -m 0644 "$OUT/dex/classes.dex" "$DEST/set-wallpaper.dex"
echo "installed $DEST/set-wallpaper.dex"
