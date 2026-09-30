#!/usr/bin/env bash
# Build the local VirGL library used by the QEMU launcher.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BUILD="$ROOT/out/virglrenderer-build"
source "$ROOT/scripts/env.sh"
export PKG_CONFIG_PATH="$EPOXY_PREFIX/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
ARGS=(setup)
[[ ! -f "$BUILD/build.ninja" ]] || ARGS+=(--reconfigure --clearcache)
if [[ "$HOST_OS" == darwin ]]; then
  ARGS+=('-Dplatforms=[]')
else
  ARGS+=('-Dplatforms=egl')
fi
meson "${ARGS[@]}" "$BUILD" "$ROOT/thirdparty/virglrenderer" \
  --pkg-config-path="$PKG_CONFIG_PATH" \
  --prefix="$VIRGL_PREFIX" --libdir=lib --buildtype=release \
  -Dtests=false -Dvenus=false
ninja -C "$BUILD" -j"${JOBS:-8}"
meson install -C "$BUILD" --no-rebuild
