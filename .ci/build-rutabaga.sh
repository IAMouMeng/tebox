#!/usr/bin/env bash
# Build rutabaga QEMU
set -euo pipefail
source "$(dirname "$0")/../scripts/env.sh"

RUTABAGA_REPO="${RUTABAGA_REPO:-https://github.com/magma-gpu/rutabaga_gfx.git}"
RUTABAGA_REF="${RUTABAGA_REF:-main}"
RUTABAGA_SRC="${RUTABAGA_SRC:-$ROOT/thirdparty/rutabaga_gfx}"
# gfxstream is kept in tree (thirdparty/gfxstream, upstream 07ee40e plus local
# fixes marked "tebox (local)"); it is not cloned or patched here.
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

clone_or_update() { # repo ref dest
  # Pinned submodule (the default paths): build exactly the recorded commit.
  local rel="${3#"$ROOT"/}"
  if [[ "$rel" != "$3" && "$(git -C "$ROOT" ls-files -s -- "$rel" | cut -c1-6)" == 160000 ]]; then
    git -C "$ROOT" submodule update --init --depth 1 -- "$rel"
    return
  fi
  # A source tree without git (e.g. a Docker build context): build it as is.
  if [[ -n "$(ls -A "$3" 2>/dev/null)" && ! -e "$3/.git" ]]; then
    echo "using $3"
    return
  fi
  if [[ -d "$3/.git" ]]; then
    echo "reusing $3"
    git -C "$3" fetch --depth 1 origin "$2" 2>/dev/null || true
    git -C "$3" checkout "$2" 2>/dev/null || true
  else
    mkdir -p "$(dirname "$3")"
    echo "cloning $1 ($2) -> $3"
    git clone --depth 1 --branch "$2" "$1" "$3"
  fi
}

[[ -f "$GFXSTREAM_SRC/meson.build" ]] || { echo "missing gfxstream sources at $GFXSTREAM_SRC" >&2; exit 1; }
clone_or_update "$RUTABAGA_REPO" "$RUTABAGA_REF" "$RUTABAGA_SRC"

# Local fixes on top of the pinned rutabaga submodule (.ci/patches/<project>-*.patch);
# a patch that no longer applies cleanly is assumed to be applied already.
apply_patches() { # project src
  local p
  for p in "$ROOT"/.ci/patches/"$1"-*.patch; do
    [[ -f "$p" ]] || continue
    if git -C "$2" apply --check "$p" 2>/dev/null; then
      git -C "$2" apply "$p" && echo "applied $(basename "$p")"
    fi
  done
}
apply_patches rutabaga "$RUTABAGA_SRC"

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
