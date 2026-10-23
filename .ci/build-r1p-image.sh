#!/usr/bin/env bash
# Build the products-only tebox image (root Dockerfile) and write
# dist/tebox-r1p-linux-arm64.tar.gz for CI / local export.
#
# Steps: host QEMU+VirGL(+rutabaga) → tebox-builder → scripts/r1p-deploy.sh build
# → docker save. Guest system/vendor/initramfs are not baked in.
#
# Env:
#   VARIANT (default from guest.lock / aosp_arm64-…)
#   TEBOX_VERSION (1.0.0), R1P_APT_MIRROR (empty = Debian),
#   R1P_SAVE_IMAGE (1), R1P_BUILD_BUILDER (1)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/scripts/env.sh"

if [[ -z "${VARIANT:-}" && -f "$ROOT/.ci/guest.lock.json" ]]; then
  VARIANT="$(python3 -c 'import json;print(json.load(open("'"$ROOT"'/.ci/guest.lock.json"))["variant"])')"
fi
VARIANT="${VARIANT:-aosp_arm64-BP4A.251205.006}"
export VARIANT
AOSP="$ROOT/src/aosp/$VARIANT"
KID="$(tr -d '[:space:]' < "$AOSP/KERNEL")"
TEBOX_VERSION="${TEBOX_VERSION:-1.0.0}"
R1P_APT_MIRROR="${R1P_APT_MIRROR-}"
R1P_BUILDER="${R1P_BUILDER:-tebox-builder}"
R1P_BUILD_BUILDER="${R1P_BUILD_BUILDER:-1}"
R1P_SAVE_IMAGE="${R1P_SAVE_IMAGE:-1}"
BIN="$ROOT/out/r1p-bin"
DIST="$ROOT/dist"
OUT_TAR="$DIST/tebox-r1p-linux-arm64.tar.gz"

command -v docker >/dev/null || { echo "docker required" >&2; exit 1; }
command -v go >/dev/null || { echo "go required" >&2; exit 1; }
[[ -f "$AOSP/KERNEL" ]] || {
  echo "missing $AOSP/KERNEL (checkout thin variant metadata first)" >&2
  exit 1
}

# GKI Image (gitignored); modules stay tracked.
bash "$ROOT/scripts/ensure-guest-downloads.sh" "$VARIANT"

# Host graphics stack + QEMU into prebuilts/, then stage under out/r1p-bin.
if [[ "$HOST_ARCH" != arm64 ]]; then
  echo "R1P image build needs an ARM64 host (got $HOST_ID)" >&2
  exit 1
fi
bash "$ROOT/.ci/build-libepoxy.sh"
VIRGL_VIDEO=1 VIRGL_VIDEO_BACKEND=v4l2 bash "$ROOT/.ci/build-virglrenderer.sh"
bash "$ROOT/.ci/build-rutabaga.sh"
RUTABAGA=1 VIRGL_VIDEO=1 QEMU_INSTALL=1 bash "$ROOT/.ci/build-qemu.sh"
mkdir -p "$BIN/host"
rsync -a --delete "$HOST_PRE/" "$BIN/host/"

if [[ "$R1P_BUILD_BUILDER" == "1" ]] || ! docker image inspect "$R1P_BUILDER" >/dev/null 2>&1; then
  docker build --platform linux/arm64 \
    -f "$ROOT/.ci/Dockerfile.tebox-builder" \
    --build-arg "APT_MIRROR=$R1P_APT_MIRROR" \
    -t "$R1P_BUILDER" "$ROOT"
fi

export TEBOX_VERSION R1P_BUILDER R1P_APT_MIRROR
# Prefer tracked scripts/r1p-deploy.sh; fall back to local out/ copy.
DEPLOY="$ROOT/scripts/r1p-deploy.sh"
[[ -x "$DEPLOY" ]] || DEPLOY="$ROOT/out/r1p-deploy.sh"
bash "$DEPLOY" build

IMG="tebox:${TEBOX_VERSION}"
if [[ -n "${GITHUB_SHA:-}" ]]; then
  docker tag "$IMG" "tebox-r1p:${GITHUB_SHA}"
fi

if [[ "$R1P_SAVE_IMAGE" == "1" ]]; then
  mkdir -p "$DIST"
  echo "saving $OUT_TAR"
  docker save "$IMG" | gzip -1 >"$OUT_TAR"
  ls -lh "$OUT_TAR"
fi

echo "built $IMG (variant metadata=$VARIANT kernel=$KID)"
