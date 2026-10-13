#!/usr/bin/env bash
# QTI Performance JNI replacement for ColorOS on QEMU (system.img untouched).
#
# ColorOS calls com.qualcomm.qti.Performance natives (QPerformance.jar is on
# the boot classpath) but zygote only loads Qualcomm's libqti_performance on
# a real Snapdragon (ro.boot.hardware=qcom*), and pretending to be one hangs
# system_server. zygote always preloads libcompiler_rt.so through the boot
# class loader, so our libcompiler_rt.so (system overlay, bind-mounted by the
# initramfs) registers no-op Performance natives in JNI_OnLoad.
#
# Trade-off: it drops the compiler-rt builtins of the original. No library
# here links against it (no DT_NEEDED); only RenderScript's on-device kernel
# linking (deprecated since Android 12) would miss it. The original cannot be
# chained: zygote's linker namespace does not see /vendor.
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
VARIANT_DIR=$(cd "$HERE/../.." && pwd)
source "$ROOT/scripts/env.sh"
CC="$NDK/toolchains/llvm/prebuilt/$NDK_HOST_TAG/bin/aarch64-linux-android34-clang"
OUT="$ROOT/out/qti-perf-stub"
OVERLAY="$VARIANT_DIR/qemu/init/system-overlay/system/lib64"
mkdir -p "$OUT" "$OVERLAY"
"$CC" -shared -fPIC -O2 -Wall -Wextra -Wl,-soname,libcompiler_rt.so \
  -o "$OUT/libcompiler_rt.so" "$HERE/qti_performance_stub.c"
install -m 0644 "$OUT/libcompiler_rt.so" "$OVERLAY/libcompiler_rt.so"
echo "installed $OVERLAY/libcompiler_rt.so"
