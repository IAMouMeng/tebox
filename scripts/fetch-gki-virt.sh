#!/usr/bin/env bash
# Fetch shared GKI Image + virtual-device modules into src/kernel/<id>/.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VERSION="${VERSION:-6.1}"
BRANCH="${BRANCH:-android14-gsi}"
KID="${1:-gki-${VERSION}-${BRANCH}}"
OUT="$ROOT/src/kernel/$KID"
GKI_OUT="$OUT/gki"
MOD_DIR="$OUT/vendor_modules"
TMP="$ROOT/out/gki-prebuilts-tmp"

mkdir -p "$GKI_OUT" "$MOD_DIR"
rm -rf "$TMP"
mkdir -p "$TMP"

clone_pinned() {
  local url=$1 dest=$2 rev=${3:-}
  if [[ -n "$rev" ]]; then
    echo "clone $url @ $rev → $dest"
    mkdir -p "$dest"
    git -C "$dest" init -q
    git -C "$dest" remote add origin "$url"
    git -C "$dest" fetch --depth 1 origin "$rev"
    git -C "$dest" checkout --detach FETCH_HEAD
  else
    echo "clone $url -b $BRANCH → $dest"
    git clone --depth 1 -b "$BRANCH" "$url" "$dest"
  fi
}

echo "clone VERSION=$VERSION BRANCH=$BRANCH → $OUT"
clone_pinned \
  "https://android.googlesource.com/kernel/prebuilts/${VERSION}/arm64" \
  "$TMP/img" "${GKI_REV:-}"
clone_pinned \
  "https://android.googlesource.com/kernel/prebuilts/common-modules/virtual-device/${VERSION}/arm64" \
  "$TMP/mod" "${MODULES_REV:-}"

[[ -f "$GKI_OUT/Image" && ! -f "$GKI_OUT/Image.prev" ]] && cp "$GKI_OUT/Image" "$GKI_OUT/Image.prev"
cp -f "$TMP/img/kernel-${VERSION}" "$GKI_OUT/Image"

MODULES=(
  virtio-gpu.ko virtio_dma_buf.ko virtio_pci.ko
  virtio_pci_legacy_dev.ko virtio_pci_modern_dev.ko virtio_mmio.ko
  virtio_blk.ko virtio_net.ko virtio_console.ko virtio_input.ko
  virtio_balloon.ko virtio_snd.ko virtio-rng.ko
  drm_dma_helper.ko vkms.ko failover.ko net_failover.ko system_heap.ko
)
for m in "${MODULES[@]}"; do
  cp -f "$TMP/mod/$m" "$MOD_DIR/"
done

cat > "$MOD_DIR/modules.load" <<'EOF'
virtio_pci_modern_dev.ko
virtio_pci_legacy_dev.ko
virtio_pci.ko
virtio_mmio.ko
virtio_dma_buf.ko
drm_dma_helper.ko
virtio-gpu.ko
virtio_blk.ko
failover.ko
net_failover.ko
virtio_net.ko
virtio_console.ko
virtio_input.ko
virtio_balloon.ko
virtio-rng.ko
virtio_snd.ko
system_heap.ko
EOF

cat > "$OUT/FETCHED_FROM.txt" <<EOF
VERSION=$VERSION
BRANCH=$BRANCH
via=git-clone

# gki/Image — prebuilt GKI kernel binary:
PREBUILT_IMAGE=https://android.googlesource.com/kernel/prebuilts/${VERSION}/arm64
#   git clone -b $BRANCH --depth 1 \\
#     https://android.googlesource.com/kernel/prebuilts/${VERSION}/arm64

# vendor_modules/*.ko — virtual-device modules (virtio-gpu/blk/net/...):
PREBUILT_MODULES=https://android.googlesource.com/kernel/prebuilts/common-modules/virtual-device/${VERSION}/arm64
#   git clone -b $BRANCH --depth 1 \\
#     https://android.googlesource.com/kernel/prebuilts/common-modules/virtual-device/${VERSION}/arm64
# Local install path: vendor_modules/ (see vendor_modules/modules.load)

# Kernel source (not cloned here; clone this if you want to build/modify GKI):
KERNEL_COMMON=https://android.googlesource.com/kernel/common
KERNEL_COMMON_BRANCH=android14-${VERSION}
#   git clone -b android14-${VERSION} --depth 1 \\
#     https://android.googlesource.com/kernel/common gki-common
EOF
rm -rf "$TMP"
file "$GKI_OUT/Image" "$MOD_DIR/virtio-gpu.ko"
ls -lh "$GKI_OUT/Image" "$MOD_DIR/virtio-gpu.ko"
echo "GKI id: $KID  (point AOSP variants at it via src/aosp/<variant>/KERNEL)"
