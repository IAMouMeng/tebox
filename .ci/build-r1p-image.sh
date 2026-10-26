#!/usr/bin/env bash
# Build the products-only tebox image (root Dockerfile) and write
# dist/tebox-r1p-linux-arm64.tar.gz for CI / local export.
#
# Host QEMU/VirGL/rutabaga are compiled inside tebox-builder (linux/arm64),
# not on the Actions runner — avoids runner-specific host toolchain failures.
# Guest system/vendor/initramfs are not baked in.
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
HOST_BUILD="$ROOT/out/r1p-host-build"
DIST="$ROOT/dist"
OUT_TAR="$DIST/tebox-r1p-linux-arm64.tar.gz"

command -v docker >/dev/null || { echo "docker required" >&2; exit 1; }
command -v go >/dev/null || { echo "go required" >&2; exit 1; }
command -v rsync >/dev/null || { echo "rsync required" >&2; exit 1; }
docker info >/dev/null 2>&1 || { echo "docker daemon not reachable" >&2; exit 1; }
[[ -f "$AOSP/KERNEL" ]] || {
  echo "missing $AOSP/KERNEL (checkout thin variant metadata first)" >&2
  exit 1
}

# Products image does not bake system.img — only the GKI Image (+ tracked *.ko).
if [[ ! -x "$ROOT/thirdparty/ffmpeg/configure" ]]; then
  echo "initializing thirdparty/ffmpeg submodule (needed for tebox-cast)"
  git -C "$ROOT" submodule update --init --depth 1 thirdparty/ffmpeg
fi
[[ -x "$ROOT/thirdparty/ffmpeg/configure" ]] || {
  echo "missing thirdparty/ffmpeg/configure after submodule init" >&2
  exit 1
}

GUEST_SKIP_SYSTEM=1 bash "$ROOT/scripts/ensure-guest-downloads.sh" "$VARIANT"
[[ -f "$ROOT/src/kernel/$KID/gki/Image" ]] || {
  echo "missing GKI Image after download" >&2
  exit 1
}

if [[ "$R1P_BUILD_BUILDER" == "1" ]] || ! docker image inspect "$R1P_BUILDER" >/dev/null 2>&1; then
  docker build --platform linux/arm64 \
    -f "$ROOT/.ci/Dockerfile.tebox-builder" \
    --build-arg "APT_MIRROR=$R1P_APT_MIRROR" \
    -t "$R1P_BUILDER" "$ROOT"
fi

# Compile linux-arm64 host stack inside the builder (V4L2 VirGL + rutabaga + QEMU).
mkdir -p "$BIN/host" "$HOST_BUILD" "$ROOT/scripts"
echo "building host QEMU/VirGL/rutabaga in $R1P_BUILDER"
# scripts/env.sh sources build/make/core/config.sh — must be visible in /src.
# qemu + gfxstream/rutabaga mounts are RW: Meson wrap-git / packagecache
# write under those source trees (e.g. qemu/subprojects/keycodemapdb).
docker run --rm --platform linux/arm64 \
  -e "JOBS=${JOBS:-4}" \
  -v "$ROOT/.ci:/src/.ci:ro" \
  -v "$ROOT/scripts/env.sh:/src/scripts/env.sh:ro" \
  -v "$ROOT/build/make/core:/src/build/make/core:ro" \
  -v "$ROOT/qemu:/src/qemu" \
  -v "$ROOT/thirdparty/libepoxy:/src/thirdparty/libepoxy:ro" \
  -v "$ROOT/thirdparty/virglrenderer:/src/thirdparty/virglrenderer:ro" \
  -v "$ROOT/thirdparty/gfxstream:/src/thirdparty/gfxstream" \
  -v "$ROOT/thirdparty/rutabaga_gfx:/src/thirdparty/rutabaga_gfx" \
  -v "$BIN/host:/src/prebuilts/host/linux-arm64" \
  -v "$HOST_BUILD:/src/out" \
  -w /src "$R1P_BUILDER" bash -c '
    set -euo pipefail
    test -f /src/build/make/core/config.sh
    bash .ci/build-libepoxy.sh
    VIRGL_VIDEO=1 VIRGL_VIDEO_BACKEND=v4l2 bash .ci/build-virglrenderer.sh
    bash .ci/build-rutabaga.sh
    RUTABAGA=1 VIRGL_VIDEO=1 QEMU_INSTALL=1 bash .ci/build-qemu.sh
    test -x prebuilts/host/linux-arm64/qemu/bin/qemu-system-aarch64
  '
[[ -x "$BIN/host/qemu/bin/qemu-system-aarch64" ]] || {
  echo "missing $BIN/host/qemu/bin/qemu-system-aarch64 after container host build" >&2
  exit 1
}

export TEBOX_VERSION R1P_BUILDER R1P_APT_MIRROR
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
