#!/usr/bin/env bash
# Assemble a small products-only tebox-r1p image and deploy to R1P.
# Dockerfile at repo root; this script lives under out/ (not scripts/).
#
# Usage: out/r1p-deploy.sh [build|push|start|stop|status|logs|export|all]
#
# Ships: stripped QEMU, tebox-api, tebox-cast + minimal FFmpeg, GKI Image,
#        CIX Mali debs, boot scripts. No sources, no AOSP *.img.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
# shellcheck source=/dev/null
source "$ROOT/scripts/env.sh"
VARIANT="${VARIANT:-aosp_arm64-BP4A.251205.006}"
AOSP_META=""
for d in "$ROOT/src/aosp/$VARIANT" "$ROOT/out/r1p-context/runtime-src/aosp/$VARIANT"; do
  [[ -f "$d/KERNEL" ]] && { AOSP_META="$d"; break; }
done
[[ -n "$AOSP_META" ]] || { echo "missing $VARIANT KERNEL metadata" >&2; exit 1; }
AOSP="$AOSP_META"
KID="$(tr -d '[:space:]' < "$AOSP_META/KERNEL")"
R1P_HOST="${R1P_HOST:-user@10.10.2.142}"
R1P_DIR="${R1P_DIR:-/mmc/tebox}"
R1P_NAME="${R1P_NAME:-tebox-1}"
R1P_ADB_PORT="${R1P_ADB_PORT:-5555}"
R1P_GRPC_PORT="${R1P_GRPC_PORT:-50051}"
R1P_CAST_PORT="${R1P_CAST_PORT:-9901}"
R1P_AUDIO_PORT="${R1P_AUDIO_PORT:-9904}"
R1P_CONTROL_PORT="${R1P_CONTROL_PORT:-9905}"
R1P_IMAGE="${R1P_IMAGE:-tebox-r1p:latest}"
R1P_BASE_IMAGE="${R1P_BASE_IMAGE:-$R1P_IMAGE}"
R1P_USER_IMAGES="${R1P_USER_IMAGES:-$R1P_DIR/user-images}"
R1P_INSTANCE_ROOT="${R1P_INSTANCE_ROOT:-$R1P_DIR/instances}"
R1P_DRIVERS="${R1P_DRIVERS:-cix-8180}"
R1P_APT_MIRROR="${R1P_APT_MIRROR-http://mirrors.ustc.edu.cn}"
R1P_BUILDER="${R1P_BUILDER:-tebox-r1p-builder}"
STAGE="$ROOT/out/r1p-docker"
BIN="$ROOT/out/r1p-bin"
FFMPEG_OUT="$ROOT/out/r1p-ffmpeg"

remote() {
  ssh "$R1P_HOST" "export SUDO_ASKPASS=\${R1P_SUDO_ASKPASS:-\$HOME/.askpass}; $*"
}

# Minimal shared FFmpeg for tebox-cast (V4L2 is primary encode on R1P).
build_min_ffmpeg_cast() {
  if [[ -x "$FFMPEG_OUT/lib/libavcodec.so" && -x "$BIN/service/tebox-cast" ]]; then
    echo "reuse $FFMPEG_OUT + tebox-cast"
    return 0
  fi
  docker image inspect "$R1P_BUILDER" >/dev/null 2>&1 || {
    echo "missing $R1P_BUILDER (needed to build minimal FFmpeg/cast)" >&2
    exit 1
  }
  mkdir -p "$FFMPEG_OUT" "$BIN/service" "$ROOT/out/r1p-ffmpeg-build"
  docker run --rm --platform linux/arm64 \
    -v "$ROOT/thirdparty/ffmpeg:/src/thirdparty/ffmpeg:ro" \
    -v "$ROOT/src/service/cast:/src/src/service/cast:ro" \
    -v "$FFMPEG_OUT:/out/ffmpeg" \
    -v "$BIN/service:/out/service" \
    -v "$ROOT/out/r1p-ffmpeg-build:/build" \
    -w /build "$R1P_BUILDER" bash -c '
      set -euo pipefail
      if [[ ! -f /out/ffmpeg/lib/libavcodec.so ]]; then
        /src/thirdparty/ffmpeg/configure \
          --prefix=/out/ffmpeg \
          --disable-static --enable-shared \
          --disable-doc --disable-debug --disable-network \
          --disable-programs --disable-autodetect \
          --disable-avdevice --disable-avformat --disable-avfilter \
          --disable-everything \
          --enable-avcodec --enable-avutil --enable-swscale --enable-swresample \
          --enable-parser=h264,hevc,aac,mjpeg \
          --enable-decoder=h264,hevc,aac,mjpeg \
          --enable-encoder=aac,mjpeg \
          --enable-filter=scale,aresample
        make -j"$(nproc)"
        make install
        # Keep runtime .so only (drop headers/pkgconfig/examples from the stage copy later).
      fi
      rm -rf /tmp/cast-out && mkdir -p /tmp/cast-out /out/service
      make -C /src/src/service/cast \
        ROOT=/src OUT=/tmp/cast-out \
        CAST_SYSTEM_FFMPEG=1 \
        PKG_CONFIG_PATH=/out/ffmpeg/lib/pkgconfig \
        CFLAGS="-O2 -Wall -Iinclude -Isrc" \
        LDFLAGS="-Wl,-rpath,/opt/tebox/lib"
      strip --strip-unneeded /tmp/cast-out/tebox-cast
      cp -L /tmp/cast-out/tebox-cast /out/service/tebox-cast
    '
}

slim_tebox() {
  local t="$1"
  local q="$t/prebuilts/host/linux-arm64/qemu"
  local host="$t/prebuilts/host/linux-arm64"

  # QEMU: binary + efi-virtio.rom only, stripped.
  mkdir -p "$q/bin" "$q/share/qemu"
  find "$q/bin" -type f ! -name qemu-system-aarch64 -delete 2>/dev/null || true
  # Keep virtio ROM before wiping the rest of share/.
  if [[ ! -f "$q/share/qemu/efi-virtio.rom" ]]; then
    find "$STAGE/tebox" "$STAGE/.libbak" -name efi-virtio.rom 2>/dev/null | head -1 \
      | while read -r rom; do cp -a "$rom" "$q/share/qemu/efi-virtio.rom"; done
  fi
  # If still missing, pull from the base image extract left under prebuilts before wipe.
  local rom_src
  rom_src="$(find "$t/prebuilts" -name efi-virtio.rom 2>/dev/null | head -1 || true)"
  [[ -n "$rom_src" && ! -f "$q/share/qemu/efi-virtio.rom" ]] && cp -a "$rom_src" "$q/share/qemu/efi-virtio.rom"
  find "$q/share/qemu" -mindepth 1 ! -name efi-virtio.rom -delete 2>/dev/null || true
  rm -rf "$q/include" "$q/var" "$q/libexec" "$q/lib"
  # Strip debug symbols (121MB → ~35MB).
  docker run --rm --platform linux/arm64 \
    -v "$q/bin/qemu-system-aarch64:/q" debian:trixie-slim \
    bash -c 'apt-get update -qq && apt-get install -y -qq binutils >/dev/null && strip --strip-unneeded /q'

  # Drop duplicate/build trees; keep runtime .so in /opt/tebox/lib only.
  rm -rf "$host/libepoxy" "$host/virglrenderer" "$host/rutabaga" "$host/gfxstream" 2>/dev/null || true

  # Minimal FFmpeg + GPU libs only (preserve symlinks — never cp -L duplicates).
  local lib="$t/lib"
  rm -rf "$lib"
  mkdir -p "$lib"
  cp -a "$FFMPEG_OUT"/lib/libavcodec.so* "$FFMPEG_OUT"/lib/libavutil.so* \
    "$FFMPEG_OUT"/lib/libswscale.so* "$FFMPEG_OUT"/lib/libswresample.so* "$lib/" 2>/dev/null || true
  if [[ -d "$STAGE/.libbak" ]]; then
    local f real dest link
    # One real file per lib family; soname symlinks must not overwrite that file.
    # Include SDL/X11/audio/.so that QEMU was linked against (avoid apt mesa).
    for f in libepoxy.so libvirglrenderer.so librutabaga_gfx_ffi.so libgfxstream_backend.so \
             libpixman-1.so libpng16.so libjpeg.so libfdt.so libslirp.so \
             libzstd.so libz.so libgbm.so \
             libSDL2-2.0.so libX11.so libXext.so libXcursor.so libXrandr.so \
             libXinerama.so libXi.so libXss.so libXrender.so libXfixes.so \
             libxcb.so libXau.so libXdmcp.so libasound.so libpulse.so \
             libpulsecommon-17.0.so libsndio.so libsamplerate.so; do
      real="$(find "$STAGE/.libbak" -maxdepth 1 -type f -name "${f}*" -exec ls -S {} + 2>/dev/null | head -1 || true)"
      [[ -n "$real" ]] || continue
      dest="$(basename "$real")"
      cp -a "$real" "$lib/$dest"
      for link in "$f" "${f}.0" "${f}.1"; do
        case "$f" in
          libepoxy.so) [[ "$link" == libepoxy.so || "$link" == libepoxy.so.0 ]] || continue ;;
          libvirglrenderer.so) [[ "$link" == libvirglrenderer.so || "$link" == libvirglrenderer.so.1 ]] || continue ;;
          libgfxstream_backend.so) [[ "$link" == libgfxstream_backend.so || "$link" == libgfxstream_backend.so.0 ]] || continue ;;
          librutabaga_gfx_ffi.so) [[ "$link" == librutabaga_gfx_ffi.so ]] || continue ;;
          *) [[ "$link" == "$f" ]] || continue ;;
        esac
        [[ "$link" == "$dest" ]] && continue
        ln -sfn "$dest" "$lib/$link"
      done
    done
  fi

  rm -rf "$t/src/aosp/$VARIANT/images"
  find "$t/src" -name '*.img' -delete 2>/dev/null || true
  # scrcpy / unused prebuilts
  rm -rf "$t/prebuilts/android-arm64" 2>/dev/null || true
}

stage_products() {
  bash "$ROOT/scripts/fetch-host-drivers.sh" "$R1P_DRIVERS"
  build_min_ffmpeg_cast

  rm -rf "$STAGE"
  mkdir -p "$STAGE/host-drivers" "$STAGE/tebox"

  docker image inspect "$R1P_BASE_IMAGE" >/dev/null 2>&1 || {
    echo "missing base image $R1P_BASE_IMAGE" >&2
    exit 1
  }
  local cid
  cid="$(docker create --platform linux/arm64 "$R1P_BASE_IMAGE")"
  docker cp "$cid:/opt/tebox/." "$STAGE/tebox/"
  docker rm -f "$cid" >/dev/null

  # Keep a copy of libs before slim for GPU .so harvest.
  rm -rf "$STAGE/.libbak"
  mkdir -p "$STAGE/.libbak"
  cp -a "$STAGE/tebox/lib/." "$STAGE/.libbak/" 2>/dev/null || true
  # Also harvest from prebuilts trees before slim deletes them.
  find "$STAGE/tebox/prebuilts/host/linux-arm64" -name '*.so*' -exec cp -aL {} "$STAGE/.libbak/" \; 2>/dev/null || true

  mkdir -p "$STAGE/tebox/bin" "$STAGE/tebox/out/service" \
    "$STAGE/tebox/scripts" "$STAGE/tebox/build/make/core" \
    "$STAGE/tebox/src/aosp/$VARIANT/qemu/cmdline" \
    "$STAGE/tebox/src/kernel/$KID/gki" \
    "$STAGE/tebox/prebuilts/host/linux-arm64/qemu/bin"

  (cd "$ROOT/src/service/api" && GOOS=linux GOARCH=arm64 CGO_ENABLED=0 \
    go build -ldflags='-s -w' -o "$STAGE/tebox/bin/tebox-api" ./cmd/tebox-api)

  local qemu_src=
  for qemu_src in "$BIN/qemu-system-aarch64" \
      "$ROOT/out/r1p-context/qemu-bin/qemu-system-aarch64" \
      "$STAGE/tebox/prebuilts/host/linux-arm64/qemu/bin/qemu-system-aarch64"; do
    [[ -x "$qemu_src" ]] && break
    qemu_src=
  done
  [[ -n "$qemu_src" ]] || { echo "missing qemu-system-aarch64 product" >&2; exit 1; }
  cp -L "$qemu_src" "$STAGE/tebox/prebuilts/host/linux-arm64/qemu/bin/qemu-system-aarch64"
  cp -L "$BIN/service/tebox-cast" "$STAGE/tebox/out/service/tebox-cast"

  cp "$ROOT/scripts/"{env.sh,boot-qemu.sh,check-runtime-inputs.py,cix-mali-env.sh,cix-r1p-start.sh} \
    "$STAGE/tebox/scripts/"
  cp "$ROOT/build/make/core/config.sh" "$STAGE/tebox/build/make/core/config.sh"
  printf '#!/bin/sh\nexec /opt/tebox/scripts/boot-qemu.sh "$@"\n' > "$STAGE/tebox/run"
  chmod +x "$STAGE/tebox/run"

  cp "$AOSP/KERNEL" "$STAGE/tebox/src/aosp/$VARIANT/"
  [[ ! -f "$AOSP/GPU" ]] || cp "$AOSP/GPU" "$STAGE/tebox/src/aosp/$VARIANT/"
  cp "$AOSP/qemu/cmdline/boot" "$STAGE/tebox/src/aosp/$VARIANT/qemu/cmdline/"
  [[ ! -f "$AOSP/qemu/vulkan-hal" ]] || cp "$AOSP/qemu/vulkan-hal" "$STAGE/tebox/src/aosp/$VARIANT/qemu/"
  cp "$ROOT/src/kernel/$KID/gki/Image" "$STAGE/tebox/src/kernel/$KID/gki/Image"

  slim_tebox "$STAGE/tebox"
  rm -rf "$STAGE/.libbak"
  cp "$ROOT/thirdparty/host-drivers/$R1P_DRIVERS"/*.deb "$STAGE/host-drivers/"

  echo "staged $(du -sh "$STAGE" | awk '{print $1}') → $STAGE"
  du -sh "$STAGE/tebox"/* 2>/dev/null | sort -hr | head -10
}

build() {
  stage_products
  docker build --platform linux/arm64 -f "$ROOT/Dockerfile" \
    --build-arg APT_MIRROR="$R1P_APT_MIRROR" \
    --build-arg VARIANT="$VARIANT" --build-arg KERNEL_ID="$KID" \
    -t "$R1P_IMAGE" "$STAGE"
  docker images "$R1P_IMAGE"
  docker run --rm --entrypoint sh "$R1P_IMAGE" -c \
    'du -sh /opt /opt/tebox /usr; ls -lh /opt/tebox/prebuilts/host/linux-arm64/qemu/bin/qemu-system-aarch64 /opt/tebox/bin/tebox-api /opt/tebox/lib/libavcodec.so* 2>/dev/null | head'
}

push() {
  remote "sudo -A rm -rf $R1P_DIR/bin"
  docker save "$R1P_IMAGE" | gzip -1 | remote 'gunzip | sudo -A docker load'
}

start() {
  stop >/dev/null 2>&1 || true
  local virgl= kernel_vol=
  remote "sudo -A test -f $R1P_DIR/bin/libvirglrenderer.so.1" && \
    virgl="-v $R1P_DIR/bin/libvirglrenderer.so.1:/opt/tebox/prebuilts/host/linux-arm64/virglrenderer/lib/libvirglrenderer.so.1:ro"
  remote "sudo -A test -f $R1P_DIR/bin/cix-r1p-start.sh" && \
    virgl+=" -v $R1P_DIR/bin/cix-r1p-start.sh:/opt/tebox/scripts/cix-r1p-start.sh:ro"
  if remote "sudo -A test -f $R1P_DIR/kernel/Image"; then
    kernel_vol="-v $R1P_DIR/kernel:/opt/tebox/src/kernel/$KID/gki:ro"
  fi
  remote "sudo -A mkdir -p $R1P_USER_IMAGES $R1P_INSTANCE_ROOT $R1P_DIR/$R1P_NAME $R1P_DIR/bin"
  remote "sudo -A docker run -d --name $R1P_NAME --restart unless-stopped --network host \
    --device /dev/kvm --device /dev/mali0 --device /dev/dri \
    --device /dev/dma_heap/system --device /dev/dma_heap/system-uncached \
    --device /dev/video0 --device /dev/video1 \
    -e ADB_PORT=$R1P_ADB_PORT -e MEM=${MEM:-4096} -e SMP=${SMP:-4} \
    -e USERDATA_MB=${USERDATA_MB:-8192} -e R1P_INSTANCE=$R1P_NAME \
    -e TEBOX_GRPC_ADDR=0.0.0.0:$R1P_GRPC_PORT -e TEBOX_CAST_ADDR=0.0.0.0:$R1P_CAST_PORT \
    -e TEBOX_AUDIO_ADDR=0.0.0.0:$R1P_AUDIO_PORT -e TEBOX_CONTROL_ADDR=0.0.0.0:$R1P_CONTROL_PORT \
    $(for kv in ${R1P_ENV:-}; do printf -- '-e %q ' "$kv"; done) \
    -v $R1P_DIR/images:/opt/tebox/src/aosp/$VARIANT/images:ro \
    $kernel_vol \
    -v $R1P_DIR/$R1P_NAME:/data \
    -v $R1P_DIR/bin:/opt/tebox/override:ro \
    -v $R1P_USER_IMAGES:$R1P_USER_IMAGES:ro \
    -v $R1P_INSTANCE_ROOT:$R1P_INSTANCE_ROOT \
    $virgl \
    $R1P_IMAGE"
  echo "gRPC ${R1P_HOST#*@}:$R1P_GRPC_PORT  cast ${R1P_HOST#*@}:$R1P_CAST_PORT  adb ${R1P_HOST#*@}:$R1P_ADB_PORT"
}

stop() { remote "sudo -A docker rm -f $R1P_NAME"; }
status() { remote "sudo -A docker ps -a --filter name=$R1P_NAME"; }
logs() { remote "sudo -A docker logs --tail ${TAIL:-200} $R1P_NAME"; }

export_img() {
  # Universal R1P base (QEMU+API+cast+kernel); not tied to one GSI variant name.
  local stamp out
  stamp="$(date +%Y%m%d)"
  out="$ROOT/out/tebox-r1p-${stamp}.tar.gz"
  echo "exporting $R1P_IMAGE → $out"
  docker save "$R1P_IMAGE" | gzip -1 > "$out"
  ls -lh "$out"
  docker run --rm --entrypoint sh "$R1P_IMAGE" -c \
    "find /opt/tebox/src -name '*.img' | grep -q . && exit 1 || true
     test -f /opt/tebox/src/kernel/$KID/gki/Image
     test -x /opt/tebox/bin/tebox-api
     du -sh /opt/tebox"
  echo "wrote $out"
}

case "${1:-}" in
  build|push|start|stop|status|logs) "$1" ;;
  export) export_img ;;
  all) build; push; start ;;
  *) echo "usage: $0 [build|push|start|stop|status|logs|export|all]" >&2; exit 2 ;;
esac
