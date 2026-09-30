#!/usr/bin/env bash
# Configure + build qemu-system-aarch64 with HVF, SDL, and virgl (virtio-gpu-gl).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/qemu"
BUILD="$ROOT/out/qemu-build"
EPOXY_PREFIX="$ROOT/out/host-deps/libepoxy"
JOBS="${JOBS:-$(sysctl -n hw.ncpu)}"

[[ -x "$SRC/configure" ]] || { echo "missing $SRC" >&2; exit 1; }
[[ -f "$EPOXY_PREFIX/include/epoxy/egl.h" ]] || {
  echo "missing epoxy+EGL at $EPOXY_PREFIX (build host libepoxy with EGL first)" >&2
  exit 1
}

export PKG_CONFIG_PATH="$EPOXY_PREFIX/lib/pkgconfig:$(brew --prefix mesa)/lib/pkgconfig:$(brew --prefix virglrenderer)/lib/pkgconfig:$(brew --prefix libepoxy)/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
export PATH="/opt/homebrew/bin:/usr/bin:/bin:$PATH"
# Prefer our epoxy headers over brew's (no egl.h)
export CPATH="$EPOXY_PREFIX/include:$(brew --prefix mesa)/include:${CPATH:-}"
export DYLD_LIBRARY_PATH="$EPOXY_PREFIX/lib:$(brew --prefix virglrenderer)/lib:$(brew --prefix mesa)/lib:${DYLD_LIBRARY_PATH:-}"

mkdir -p "$BUILD"
cd "$BUILD"

# Reconfigure if OpenGL is still off.
NEED_RECONF=0
if [[ ! -f "$BUILD/build.ninja" ]]; then
  NEED_RECONF=1
elif ! rg -q '#define CONFIG_OPENGL 1' "$BUILD/config-host.h" 2>/dev/null; then
  NEED_RECONF=1
fi

if [[ "$NEED_RECONF" == 1 ]]; then
  echo "configuring QEMU with virgl/opengl..."
  # Fresh meson reconfigure: wipe build options via configure
  "$SRC/configure" \
    --target-list=aarch64-softmmu \
    --enable-hvf \
    --enable-sdl \
    --enable-slirp \
    --enable-virglrenderer \
    --enable-opengl \
    --disable-vhost-user \
    --disable-docs \
    --enable-plugins \
    --disable-pie
fi

ninja -C "$BUILD" -j"$JOBS"
"$BUILD/qemu-system-aarch64" -device help 2>/dev/null | rg -i 'virtio-gpu' || true
if "$BUILD/qemu-system-aarch64" -device help 2>/dev/null | rg -q 'virtio-gpu-gl'; then
  echo "OK: virtio-gpu-gl available"
else
  echo "WARN: virtio-gpu-gl still missing — check CONFIG_OPENGL in config-host.h" >&2
  rg -n 'CONFIG_OPENGL|CONFIG_VIRGL' "$BUILD/config-host.h" || true
  exit 1
fi
