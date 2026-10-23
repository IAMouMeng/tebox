#!/usr/bin/env bash
# Build tebox_mali.ko for the guest GKI (android14-6.1) so it loads on the
# mirror's prebuilt Image. Runs the kernel build in a linux/amd64 container
# (OrbStack/Rosetta on Apple Silicon) = AOSP clang's native host; cross-builds
# the aarch64 module.
#
# vermagic must match the running guest exactly
# (6.1.172-android14-11-g87c0a9ca734b-ab16116086):
#   - check kernel/common out to commit $KCOMMIT  -> "-11-g87c0a9ca734b"
#   - export BUILD_NUMBER=$BUILD_NUMBER            -> "-ab16116086"
#     (scripts/setlocalversion appends -ab$BUILD_NUMBER)
#
# Prereq: a repo-synced workspace at $WS (common-android14-6.1):
#   mkdir -p out/gki-workspace && cd out/gki-workspace
#   repo init -u https://android.googlesource.com/kernel/manifest -b common-android14-6.1 --depth=1
#   repo sync -c -j8
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
# Guest runs vermagic 6.1.172-android14-11-g87c0a9ca734b-ab16116086. That exact
# commit is not reachable on stock android14-6.1, so we build the 6.1.172 tree
# (251497955f...) and force the release string via CONFIG_LOCALVERSION +
# .scmversion. KMI is frozen across android14-6.1, so KMI symbol CRCs match.
WS="${WS:-$ROOT/out/gki-workspace}"
# The guest GKI is android14-6.1.172_r00 (+ vendor patches). Build at that exact
# release merge so include/linux/module.h / struct module match the guest — a
# mismatch makes the module loader reject the .ko with a silent -ENOEXEC. The
# guest's own HEAD (87c0a9ca734b, 11 vendor commits on top) is not on AOSP;
# _r00 has the right struct module and gki_defconfig.
KCOMMIT="${KCOMMIT:-108b53924ea33cadfc5b446f4e1d841465f278f1}"  # Merge android14-6.1.172_r00
SCMVERSION="${SCMVERSION:--11-g87c0a9ca734b-ab16116086}"
LOCALVERSION="${LOCALVERSION:--android14}"
IMAGE="${IMAGE:-debian:trixie}"

[[ -d "$WS/common" ]] || { echo "missing $WS/common (run repo sync first)" >&2; exit 1; }

# Inner build script, executed inside the amd64 container.
cat > "$WS/.build-tebox.sh" <<INNER
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends \
    make bc bison flex libssl-dev libelf-dev kmod rsync python3 \
    git ca-certificates cpio zstd xz-utils gcc g++ libc6-dev >/dev/null

git config --global --add safe.directory '*'
CLANG=\$(ls -d /ws/prebuilts/clang/host/linux-x86/clang-* 2>/dev/null | grep -v stable | sort | tail -1)
[[ -n "\$CLANG" ]] || { echo "no prebuilt clang under /ws/prebuilts/clang/host/linux-x86" >&2; exit 1; }
export PATH="\$CLANG/bin:/ws/prebuilts/kernel-build-tools/linux-x86/bin:\$PATH"
MK="make LLVM=1 ARCH=arm64 O=/build/out -j\$(nproc)"

if [ -f /build/out/Module.symvers ] && [ -d /build/common ]; then
  # Fast path: the kernel tree + Module.symvers are cached in the persistent
  # /build volume from a prior run, so just rebuild the module (seconds).
  echo "reusing cached kernel build in /build/out"
  cd /build/common
else
  # First run: /build is a persistent docker volume on the container's
  # case-sensitive ext4. The /ws mount is macOS APFS (case-insensitive) and the
  # kernel tree has case-colliding files, so clone/checkout/build must happen on
  # /build, not /ws. Use /ws/common's objects as alternates (no re-download).
  rm -rf /build/common /build/out
  git clone --quiet --shared /ws/common /build/common
  cd /build/common
  git checkout -f -q "$KCOMMIT" || { echo "commit $KCOMMIT missing" >&2; exit 1; }
  # Force the exact guest release string: CONFIG_LOCALVERSION + .scmversion.
  printf '%s' "$SCMVERSION" > .scmversion
  mkdir -p /build/out
  \$MK gki_defconfig
  ./scripts/config --file /build/out/.config --set-str LOCALVERSION "$LOCALVERSION"
  \$MK olddefconfig
  # vermagic carries "modversions" (CONFIG_MODVERSIONS=y) so the module needs
  # symbol CRCs -> a full vmlinux build (Module.symvers). One-time; cached after.
  \$MK
  echo "built release: \$(cat /build/out/include/config/kernel.release)"
fi
# Build the external module against the (cached or fresh) Module.symvers.
\$MK M=/mod modules
echo "=== built module ==="
ls -l /mod/*.ko
/ws/prebuilts/clang/host/linux-x86/*/bin/llvm-readelf -p .modinfo /mod/tebox_mali.ko 2>/dev/null | grep -iE "vermagic|name=" || strings /mod/tebox_mali.ko | grep -iE "vermagic|6\.1\.172" | head
INNER
chmod +x "$WS/.build-tebox.sh"

echo "building in $IMAGE (linux/amd64)..."
# /build is a persistent named volume (case-sensitive VM ext4) so the one-time
# kernel build is cached across runs; later module rebuilds take seconds.
docker run --rm --platform linux/amd64 \
    -v "$WS:/ws" -v "$HERE:/mod" -v tebox-kbuild:/build -w /ws \
    "$IMAGE" bash /ws/.build-tebox.sh

cp -f "$HERE/tebox_mali.ko" "$HERE/tebox_mali.ko.built" 2>/dev/null || true
echo "done: $HERE/tebox_mali.ko"
