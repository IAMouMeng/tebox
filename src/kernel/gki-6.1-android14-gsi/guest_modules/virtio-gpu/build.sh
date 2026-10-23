#!/usr/bin/env bash
# Build virtio-gpu.ko for the guest GKI (android14-6.1.172) with the upstream
# v6.14 "drm/virtio: Import scanout buffers from other devices" series
# backported (patches/), so KMS-only virtio-gpu can PRIME-import the Mali
# client target and scan it out without a copy.
#
# Reuses the cached kernel build that ../tebox-mali/build.sh leaves in the
# tebox-kbuild docker volume (run that once first).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$HERE/../../../../.." && pwd)
WS="${WS:-$ROOT/out/gki-workspace}"
IMAGE="${IMAGE:-debian:trixie}"

cat > "$WS/.build-virtio-gpu.sh" <<'INNER'
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq
apt-get install -y -qq --no-install-recommends make bc bison flex libssl-dev \
    libelf-dev kmod python3 >/dev/null
[ -f /build/out/Module.symvers ] || { echo "no cached kernel build; run ../tebox-mali/build.sh" >&2; exit 1; }
CLANG=$(ls -d /ws/prebuilts/clang/host/linux-x86/clang-* | grep -v stable | sort | tail -1)
export PATH="$CLANG/bin:/ws/prebuilts/kernel-build-tools/linux-x86/bin:$PATH"
cd /build/common
make LLVM=1 ARCH=arm64 O=/build/out -j"$(nproc)" M=/mod CONFIG_DRM_VIRTIO_GPU=m \
    CONFIG_VIRTIO_DMA_SHARED_BUFFER=m modules
llvm-readelf -p .modinfo /mod/virtio-gpu.ko | grep -E "vermagic|depends"
INNER

docker run --rm --platform linux/amd64 \
    -v "$WS:/ws" -v "$HERE:/mod" -v tebox-kbuild:/build -w /ws \
    "$IMAGE" bash /ws/.build-virtio-gpu.sh
echo "done: $HERE/virtio-gpu.ko"
