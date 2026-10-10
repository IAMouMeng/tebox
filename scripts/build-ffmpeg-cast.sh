#!/usr/bin/env bash
# Build FFmpeg from thirdparty/ffmpeg for src/service/cast (bundled package).
# Installs to out/cast-ffmpeg and copies into prebuilts/host/<os-arch>/ffmpeg.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
SRC="$ROOT/thirdparty/ffmpeg"
PREFIX="${CAST_FFMPEG_PREFIX:-$ROOT/out/cast-ffmpeg}"
DEST="$HOST_PRE/ffmpeg"
JOBS="${JOBS:-$(sysctl -n hw.ncpu 2>/dev/null || nproc)}"

[[ -x "$SRC/configure" ]] || {
  echo "missing $SRC — clone FFmpeg into thirdparty/ffmpeg first" >&2
  exit 1
}

mkdir -p "$ROOT/out/cast-ffmpeg-build"
cd "$ROOT/out/cast-ffmpeg-build"

EXTRA=()
# Prefer in-tree / vendored encoder deps when Homebrew provides them for the
# *build*, but the result is installed into PREFIX and linked by tebox-cast
# via rpath — runtime does not need system ffmpeg.
ENCODERS="libx264"
CONF_LIBS=(--enable-libx264)
case "$(uname -s)" in
  Darwin)
    EXTRA+=(--enable-videotoolbox --enable-ffplay --enable-sdl2)
    ENCODERS+=",h264_videotoolbox,hevc_videotoolbox"
    for formula in x264 x265 libvpx sdl2; do
      p="$(brew --prefix "$formula" 2>/dev/null || true)"
      if [[ -n "$p" && -d "$p" ]]; then
        export PKG_CONFIG_PATH="$p/lib/pkgconfig:${PKG_CONFIG_PATH:-}"
        export CFLAGS="-I$p/include ${CFLAGS:-}"
        export LDFLAGS="-L$p/lib ${LDFLAGS:-}"
      fi
    done
    if pkg-config --exists x265 2>/dev/null; then
      CONF_LIBS+=(--enable-libx265)
      ENCODERS+=",libx265"
    fi
    if pkg-config --exists vpx 2>/dev/null; then
      CONF_LIBS+=(--enable-libvpx)
      ENCODERS+=",libvpx,libvpx-vp9"
    fi
    ;;
  Linux)
    EXTRA+=(--enable-vaapi --enable-ffplay --enable-sdl2)
    ENCODERS+=",h264_vaapi,hevc_vaapi"
    ;;
esac

CFG=(
  --prefix="$PREFIX"
  --disable-static --enable-shared
  --disable-doc --disable-debug
  --enable-gpl
  "${CONF_LIBS[@]}"
  --enable-encoder="$ENCODERS"
  --enable-decoder=h264,hevc,vp8,vp9,av1
  --enable-parser=h264,hevc,vp8,vp9,av1
  --disable-network --disable-lzma
)

if ! "$SRC/configure" "${CFG[@]}" "${EXTRA[@]}"; then
  echo "configure failed; retrying minimal (x264 + VT/ffplay)" >&2
  MIN=(--enable-libx264 --enable-encoder=libx264)
  case "$(uname -s)" in
    Darwin) MIN+=(--enable-videotoolbox --enable-encoder=libx264,h264_videotoolbox,hevc_videotoolbox --enable-ffplay --enable-sdl2) ;;
    Linux) MIN+=(--enable-ffplay --enable-sdl2) ;;
  esac
  "$SRC/configure" \
    --prefix="$PREFIX" \
    --disable-static --enable-shared \
    --disable-doc \
    --enable-gpl \
    "${MIN[@]}" \
    --disable-network --disable-lzma
fi

make -j"$JOBS"
make install

# Package layout for a single redistributable host tree.
mkdir -p "$DEST"
rsync -a --delete "$PREFIX/" "$DEST/"

echo "FFmpeg installed:"
echo "  build prefix: $PREFIX"
echo "  package copy: $DEST"
"$PREFIX/bin/ffplay" -version 2>/dev/null | head -1 || true
pkg-config --define-variable=prefix="$PREFIX" --modversion libavcodec
