#!/usr/bin/env bash
# Build rutabaga QEMU
set -euo pipefail
source "$(dirname "$0")/../scripts/env.sh"

# rutabaga_gfx (upstream 9f22cc9) and gfxstream (upstream 07ee40e) are kept in
# tree with local fixes marked "tebox (local)"; nothing is cloned or patched.
RUTABAGA_SRC="${RUTABAGA_SRC:-$ROOT/thirdparty/rutabaga_gfx}"
GFXSTREAM_SRC="${GFXSTREAM_SRC:-$ROOT/thirdparty/gfxstream}"
FEATURES="${RUTABAGA_FEATURES:-gfxstream}"

if [[ "$(uname -s)" == Darwin ]]; then
  JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"
else
  JOBS="${JOBS:-$(nproc)}"
fi

command -v meson >/dev/null 2>&1 || { echo "missing meson (pip install meson / apt install meson)" >&2; exit 1; }
command -v cargo >/dev/null 2>&1 || {
  echo "missing cargo/rust. Install rustup first:" >&2
  echo "  curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh" >&2
  exit 1
}
for hdr in drm/drm.h glm/glm.hpp stb_image.h; do :; done
if ! pkg-config --exists gbm 2>/dev/null; then
  echo "NOTE: no system gbm.pc; on Debian/Ubuntu: sudo apt install libdrm-dev libglm-dev libstb-dev mesa-common-dev" >&2
fi

for src in "$GFXSTREAM_SRC" "$RUTABAGA_SRC"; do
  [[ -f "$src/meson.build" ]] || { echo "missing sources at $src" >&2; exit 1; }
done

export PATH="$HOME/.cargo/bin:${PATH:-/usr/bin:/bin}"
export PKG_CONFIG_PATH="$RUTABAGA_PREFIX/lib/pkgconfig:$RUTABAGA_PREFIX/lib/aarch64-linux-gnu/pkgconfig:$VIRGL_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"

# If virgl_renderer feature requested, ensure virglrenderer is built first so
# dependency('virglrenderer') resolves via $VIRGL_PREFIX/lib/pkgconfig.
if [[ ",$FEATURES," == *,virgl_renderer* ]]; then
  if ! PKG_CONFIG_PATH="$PKG_CONFIG_PATH" pkg-config --exists virglrenderer 2>/dev/null; then
    echo "virglrenderer not found; building it first (scripts/build-virglrenderer.sh)..."
    "$ROOT/scripts/build-virglrenderer.sh"
  fi
fi

# 1. gfxstream host backend -> $RUTABAGA_PREFIX (provides gfxstream_backend.pc).
GFX_BUILD="$ROOT/out/gfxstream-build"
ARGS=(setup)
[[ ! -f "$GFX_BUILD/build.ninja" ]] || ARGS+=(--reconfigure)
meson "${ARGS[@]}" "$GFX_BUILD" "$GFXSTREAM_SRC" \
  --prefix="$RUTABAGA_PREFIX" --libdir=lib --buildtype=release
ninja -C "$GFX_BUILD" -j"$JOBS"
meson install -C "$GFX_BUILD" --no-rebuild

# 2. rutabaga FFI with the gfxstream (vulkan) backend.
RUTABAGA_BUILD="$ROOT/out/rutabaga-build"
ARGS=(setup)
[[ ! -f "$RUTABAGA_BUILD/build.ninja" ]] || ARGS+=(--reconfigure --clearcache)
# shellcheck disable=SC2206
FEAT_ARR=($FEATURES)
FEAT_STR=$(IFS=,; echo "${FEAT_ARR[*]}")
meson "${ARGS[@]}" "$RUTABAGA_BUILD" "$RUTABAGA_SRC" \
  --pkg-config-path="$PKG_CONFIG_PATH" \
  --prefix="$RUTABAGA_PREFIX" --libdir=lib --buildtype=release \
  "-Dfeatures=${FEAT_STR}" -Dffi=true
ninja -C "$RUTABAGA_BUILD" -j"$JOBS"
meson install -C "$RUTABAGA_BUILD" --no-rebuild

echo "installed to $RUTABAGA_PREFIX"
PKG_CONFIG_PATH="$RUTABAGA_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}" pkg-config --modversion rutabaga_gfx_ffi
pkg-config --modversion gfxstream_backend || true
echo "HINT: rebuild QEMU with RUTABAGA=1 scripts/build-qemu.sh HOST_ARCH=arm64"
