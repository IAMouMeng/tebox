#!/usr/bin/env bash
# Extract the gfxstream guest Vulkan driver (vulkan.ranchu) and its vendor
# libraries from Google's official Android 16 arm64 emulator image into
# prebuilts/android-arm64/gfxstream/, laid out like /vendor. build-vendor-img.sh merges it next to Mesa; the boot argument
# androidboot.tebox.gpu=gfxstream selects it (default: virgl).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
URL="https://dl.google.com/android/repository/sys-img/android/arm64-v8a-36_r02.zip"
SHA256="c6194eb8ace128b174ea86b5fbc0c78204147123b3b5ce6d0e3d615a06ad31d1"
DL="$ROOT/downloads/emulator"
ZIP="$DL/$(basename "$URL")"
OUT="$ROOT/prebuilts/android-arm64/gfxstream"
READELF="$ANDROID_NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/llvm-readelf"

mkdir -p "$DL"
if [[ ! -s "$ZIP" ]]; then
  curl -fL --retry 3 -o "$ZIP.part" "$URL"
  mv -f "$ZIP.part" "$ZIP"
fi
echo "$SHA256  $ZIP" | shasum -a 256 -c - >/dev/null

STAGE="$ROOT/out/gfxstream-guest"
rm -rf "$STAGE" && mkdir -p "$STAGE"
unzip -q -o "$ZIP" 'arm64-v8a/vendor.img' -d "$STAGE"
# vendor.img is a GPT disk with one ext4 partition.
read -r FIRST LAST < <(python3 - "$STAGE/arm64-v8a/vendor.img" <<'PY'
import struct, sys
f = open(sys.argv[1], 'rb'); f.seek(512); h = f.read(92)
lba, num, size = struct.unpack('<QII', h[72:88]); f.seek(lba * 512)
for _ in range(num):
    e = f.read(size); first, last = struct.unpack('<QQ', e[32:48])
    if first: print(first, last); break
PY
)
dd if="$STAGE/arm64-v8a/vendor.img" of="$STAGE/vendor.ext4" bs=512 skip="$FIRST" \
  count=$((LAST - FIRST + 1)) status=none
mkdir -p "$STAGE/vendor"
"$E2/debugfs" -R "rdump / $STAGE/vendor" "$STAGE/vendor.ext4" >/dev/null 2>&1

# Only the Vulkan driver: GLES is the GSI's ANGLE on top of it, and buffers
# come from our own allocator/mapper (minigbm-layout handles). The ranchu
# gralloc needs the emulator-only /dev/goldfish_address_space.
ROOTS=(
  lib64/hw/vulkan.ranchu.so
)
# Libraries the VirGL stack already ships (GSI copies); keep those.
KEEP_OURS="libbase.so libc++.so libcutils.so libhardware.so libutils.so libz.so"

rm -rf "$OUT" && mkdir -p "$OUT"
python3 - "$READELF" "$STAGE/vendor" "$OUT" "$KEEP_OURS" "${ROOTS[@]}" <<'PY'
import os, shutil, subprocess, sys
readelf, src, out, keep = sys.argv[1:5]
roots = sys.argv[5:]
keep = set(keep.split())
vendor_libs = set(os.listdir(os.path.join(src, 'lib64')))
todo, seen = list(roots), set()
while todo:
    rel = todo.pop()
    if rel in seen:
        continue
    seen.add(rel)
    dst = os.path.join(out, rel)
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    shutil.copy2(os.path.join(src, rel), dst)
    dyn = subprocess.run([readelf, '-d', os.path.join(src, rel)], capture_output=True, text=True).stdout
    for line in dyn.splitlines():
        if '(NEEDED)' in line:
            lib = line.split('[')[1].split(']')[0]
            if lib in vendor_libs and lib not in keep:
                todo.append('lib64/' + lib)
print(f'{len(seen)} files')
PY
echo "installed gfxstream guest stack into $OUT ($(du -sh "$OUT" | cut -f1))"
