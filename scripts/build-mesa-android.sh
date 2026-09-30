#!/usr/bin/env bash
# Cross-build guest Mesa (VirGL / virtio_gpu) for aarch64 Android → prebuilt/arm64.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SRC="$ROOT/thirdparty/mesa/src"
OUT="$ROOT/out/mesa-android"
PRE="$ROOT/thirdparty/mesa/prebuilt/arm64"
CROSS="$OUT/android-aarch64.cross"
PKGDIR="$OUT/pkgconfig"
SDK_VER="${PLATFORM_SDK_VERSION:-34}"
NDK="${ANDROID_NDK:-}"

if [[ -z "$NDK" ]]; then
  for c in \
    /opt/homebrew/Caskroom/android-ndk/*/AndroidNDK*/Contents/NDK \
    "$HOME/Library/Android/sdk/ndk/"*; do
    [[ -d "$c/toolchains/llvm/prebuilt" ]] && NDK=$c && break
  done
fi
[[ -d "$NDK" ]] || { echo "set ANDROID_NDK" >&2; exit 1; }
[[ -d "$SRC/.git" || -f "$SRC/meson.build" ]] || {
  echo "missing mesa src; run scripts/fetch-mesa-src.sh" >&2
  exit 1
}

HOST_TAG=darwin-x86_64
TC="$NDK/toolchains/llvm/prebuilt/$HOST_TAG"
[[ -d "$TC" ]] || { echo "missing NDK toolchain $TC" >&2; exit 1; }
CLANG="$TC/bin/aarch64-linux-android${SDK_VER}-clang"
CLANGXX="$TC/bin/aarch64-linux-android${SDK_VER}-clang++"
[[ -x "$CLANG" ]] || { echo "missing $CLANG" >&2; exit 1; }

export PATH="/opt/homebrew/bin:/usr/bin:/bin:$PATH"
mkdir -p "$OUT" "$PKGDIR" "$PRE"/{egl,dri,hw}

# Minimal pkg-config dir so meson is happy (android-stub avoids most deps).
# zlib often comes from NDK sysroot; provide a tiny .pc if needed.
if [[ ! -f "$PKGDIR/zlib.pc" ]]; then
  cat > "$PKGDIR/zlib.pc" <<EOF
prefix=$TC/sysroot/usr
libdir=\${prefix}/lib/aarch64-linux-android
includedir=\${prefix}/include
Name: zlib
Description: zlib
Version: 1.2.11
Libs: -lz
Cflags: -I\${includedir}
EOF
fi

cat > "$CROSS" <<EOF
[binaries]
ar = '$TC/bin/llvm-ar'
c = ['$CLANG', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables']
cpp = ['$CLANGXX', '-fno-exceptions', '-fno-unwind-tables', '-fno-asynchronous-unwind-tables', '-static-libstdc++']
c_ld = 'lld'
cpp_ld = 'lld'
strip = '$TC/bin/llvm-strip'
pkg-config = ['env', 'PKG_CONFIG_LIBDIR=$PKGDIR', '$(command -v pkg-config)']

[host_machine]
system = 'android'
cpu_family = 'aarch64'
cpu = 'armv8'
endian = 'little'

[properties]
needs_exe_wrapper = true
pkg_config_libdir = '$PKGDIR'
EOF

BUILD="$OUT/build"
if [[ "${MESA_RECONF:-0}" == "1" || ! -f "$BUILD/build.ninja" ]]; then
  rm -rf "$BUILD"
  meson setup "$BUILD" "$SRC" \
    --cross-file "$CROSS" \
    --prefix="$OUT/install" \
    --libdir=lib \
    --buildtype=release \
    -Dplatforms=android \
    -Dplatform-sdk-version="$SDK_VER" \
    -Dandroid-stub=true \
    -Dandroid-libbacktrace=disabled \
    -Degl=enabled \
    -Dgles1=enabled \
    -Dgles2=enabled \
    -Dglx=disabled \
    -Dgbm=enabled \
    -Dllvm=disabled \
    -Dshared-llvm=disabled \
    -Dcpp_rtti=false \
    -Dlmsensors=disabled \
    -Dgallium-drivers=virgl,softpipe \
    -Dvulkan-drivers= \
    -Dgallium-vdpau=disabled \
    -Dgallium-va=disabled \
    -Dgallium-xa=disabled \
    -Dvideo-codecs= \
    -Dexpat=disabled \
    -Dlibunwind=disabled \
    -Dxmlconfig=disabled
fi

ninja -C "$BUILD" -j"$(sysctl -n hw.ncpu)"
meson install -C "$BUILD" --no-rebuild

# Stage Android EGL loader names + DRI
INST="$OUT/install/lib"
mkdir -p "$PRE/egl" "$PRE/dri"
shopt -s nullglob
# EGL/GLES
for pair in \
  "libEGL.so:libEGL_mesa.so" \
  "libGLESv1_CM.so:libGLESv1_CM_mesa.so" \
  "libGLESv2.so:libGLESv2_mesa.so"; do
  src=${pair%%:*}; dst=${pair##*:}
  if [[ -f "$INST/$src" ]]; then
    cp -f "$INST/$src" "$PRE/egl/$dst"
  elif [[ -f "$INST/$dst" ]]; then
    cp -f "$INST/$dst" "$PRE/egl/$dst"
  fi
done
# glapi / gallium / dri
[[ -f "$INST/libglapi.so" ]] && cp -f "$INST/libglapi.so" "$PRE/"
for f in "$INST"/libgallium*.so "$INST"/dri/*.so "$INST"/*_dri.so; do
  [[ -f "$f" ]] || continue
  cp -f "$f" "$PRE/dri/"
done
# Also copy any remaining .so that look useful
for f in "$INST"/*.so; do
  base=$(basename "$f")
  case "$base" in
    libEGL*|libGLES*|libglapi*|libgbm*|libgallium*) cp -f "$f" "$PRE/" ;;
  esac
done
shopt -u nullglob

echo "==== staged ===="
find "$PRE" -type f | sort
file "$PRE/egl"/* "$PRE/dri"/* 2>/dev/null | head -20
echo "OK → $PRE (run scripts/build-vendor-img.sh next)"
