#!/usr/bin/env bash
# Write OriginOS system overlays into system.img so the guest has no rootfs
# bind mounts over /system. Other variants do not ship qemu/init/bake-overlays.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VARIANT="${1:?usage: bake-system-overlays.sh <variant>}"
AOSP="$ROOT/src/aosp/$VARIANT"
INIT_DIR="$AOSP/qemu/init"
IMG="$AOSP/images/system.img"
STAMP="$AOSP/images/.overlay-baked"
[[ -f "$INIT_DIR/bake-overlays" ]] || exit 0
[[ -f "$IMG" ]] || { echo "missing $IMG" >&2; exit 1; }
source "$ROOT/scripts/env.sh"
[[ -x "$E2/debugfs" ]] || { echo "need debugfs" >&2; exit 1; }

if pgrep -f "qemu-system-aarch64.*${VARIANT}" >/dev/null 2>&1; then
  echo "stop the running QEMU for $VARIANT before baking system.img" >&2
  exit 1
fi

inputs=(
  "$0"
  "$INIT_DIR/init.rc"
  "$INIT_DIR/keystore2.rc"
  "$INIT_DIR/surfaceflinger.rc"
  "$INIT_DIR/wificond"
)
need=0
if [[ ! -f "$STAMP" || "$IMG" -nt "$STAMP" ]]; then
  need=1
fi
for f in "${inputs[@]}"; do
  if [[ -f "$f" && ( ! -f "$STAMP" || "$f" -nt "$STAMP" ) ]]; then
    need=1
  fi
done
if (( need == 0 )); then
  echo "system overlays already baked for $VARIANT"
  exit 0
fi

WORKDIR="$(mktemp -d)"
trap 'rm -rf "$WORKDIR"' EXIT

patch_build_prop() {
  python3 - "$1" <<'PY'
import sys
path = sys.argv[1]
text = open(path, encoding="utf-8", errors="surrogateescape").read().splitlines()
drop = (
    "ro.kernel.qemu.gles=",
    "qemu.hw.mainkeys=",
    "qemu.hw.mainkeys.vivo=",
)
out = []
saw_adb = False
for line in text:
    if line.startswith(drop):
        continue
    if line.startswith("ro.secure="):
        line = "ro.secure=1"
    elif line.startswith("ro.debuggable="):
        line = "ro.debuggable=0"
    elif line.startswith("ro.build.product="):
        line = "ro.build.product=PD2266"
    elif line.startswith("ro.build.flavor="):
        line = "ro.build.flavor=PD2266-user"
    elif line.startswith("ro.build.description="):
        line = "ro.build.description=PD2266-user 15 AP3A.240905.015.A2 compiler250711173629 release-keys"
    elif line.startswith("ro.system.product.cpu.abilist32="):
        line = "ro.system.product.cpu.abilist32="
    elif line.startswith("ro.system.product.cpu.abilist="):
        line = "ro.system.product.cpu.abilist=arm64-v8a"
    elif line.startswith("ro.adb.secure="):
        line = "ro.adb.secure=0"
        saw_adb = True
    out.append(line)
if not saw_adb:
    out.append("ro.adb.secure=0")
open(path, "w", encoding="utf-8").write("\n".join(out) + "\n")
PY
}

patch_vndk() {
  python3 - "$1" <<'PY'
import sys
path = sys.argv[1]
lines = open(path, encoding="utf-8", errors="surrogateescape").read().splitlines(keepends=True)
out = []
skip = False
for line in lines:
    if line.startswith("on property:persist.sys.phh.mainkeys="):
        skip = True
        continue
    if skip:
        if line.startswith("    setprop qemu.hw.mainkeys"):
            skip = False
            continue
        skip = False
    out.append(line)
open(path, "w", encoding="utf-8").writelines(out)
PY
}

# S_IFREG | mode. debugfs set_inode_field replaces i_mode, including the type bits.
bake_one() {
  local host=$1 dest=$2 mode=$3 uid=$4 gid=$5 ctx=$6
  local cmds="$WORKDIR/cmds"
  cat > "$cmds" <<EOF
rm $dest
write $host $dest
set_inode_field $dest mode $mode
set_inode_field $dest uid $uid
set_inode_field $dest gid $gid
ea_set $dest security.selinux $ctx
EOF
  "$E2/debugfs" -w -f "$cmds" "$IMG"
  local kind
  kind=$("$E2/debugfs" -R "stat $dest" "$IMG" 2>/dev/null | awk '/Type:/ { for (i = 1; i <= NF; i++) if ($i == "Type:") { print $(i + 1); exit } }')
  [[ "$kind" == regular ]] || { echo "bake failed for $dest (type=$kind)" >&2; exit 1; }
  echo "baked $dest"
}

"$E2/debugfs" -R "dump system/build.prop $WORKDIR/build.prop" "$IMG" >/dev/null
[[ -s "$WORKDIR/build.prop" ]] || { echo "failed to read system/build.prop" >&2; exit 1; }
patch_build_prop "$WORKDIR/build.prop"
bake_one "$WORKDIR/build.prop" system/build.prop 0100600 0 0 u:object_r:system_file:s0

"$E2/debugfs" -R "dump system/etc/init/vndk.rc $WORKDIR/vndk.rc" "$IMG" >/dev/null
if [[ -s "$WORKDIR/vndk.rc" ]] && grep -q 'qemu.hw.mainkeys' "$WORKDIR/vndk.rc"; then
  patch_vndk "$WORKDIR/vndk.rc"
  bake_one "$WORKDIR/vndk.rc" system/etc/init/vndk.rc 0100644 0 0 u:object_r:system_file:s0
fi

bake_one "$INIT_DIR/init.rc" system/etc/init/hw/init.rc 0100644 0 0 u:object_r:system_file:s0
bake_one "$INIT_DIR/keystore2.rc" system/etc/init/keystore2.rc 0100644 0 0 u:object_r:system_file:s0
bake_one "$INIT_DIR/surfaceflinger.rc" system/etc/init/surfaceflinger.rc 0100644 0 0 u:object_r:system_file:s0
bake_one "$INIT_DIR/wificond" system/bin/wificond 0100755 0 2000 u:object_r:wificond_exec:s0

# apexd's packed mtime is the 2009 Android epoch. Use the GSI build date.
APEX_CMDS="$WORKDIR/apex"
cat > "$APEX_CMDS" <<'EOF'
set_inode_field system/bin/apexd mtime 1752226589
set_inode_field system/bin/apexd ctime 1752226589
set_inode_field system/bin/apexd atime 1752226589
EOF
"$E2/debugfs" -w -f "$APEX_CMDS" "$IMG"

touch "$STAMP"
echo "baked system overlays into $IMG"
