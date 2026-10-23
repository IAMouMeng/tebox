#!/usr/bin/env bash
# Build the CIX Sky1 (MYT R1P) runtime image locally and run it on a remote
# R1P over SSH. Only binaries and runtime images leave this machine. The
# container runs tebox-api (gRPC 50051, video 9901, audio 9904, control 9905
# TCP + UDP input).
#
# Usage: scripts/r1p-deploy.sh [build|push|images|start|stop|status|logs|export|all|bin]
#   bin: rebuild only tebox-api, tebox-cast and VirGL, upload them,
#        boot-qemu.sh, cix-r1p-start.sh and the variant cmdline to $R1P_DIR/bin (mounted over the image's copies) and
#        restart the container. Needs the tebox-r1p-builder image that
#        `build` tags; no image transfer.
#   export: docker save | gzip → out/tebox-r1p-<variant>-<date>.tar.gz
#           (QEMU+API+cast+kernel; no AOSP *.img)
# Env:   R1P_HOST (user@10.10.2.142)  R1P_DIR (/mmc/tebox)  R1P_NAME (tebox-1)
#        R1P_ADB_PORT (5555)  R1P_SUDO_ASKPASS (~/.askpass on the remote)
#        R1P_GRPC_PORT (50051) R1P_CAST_PORT (9901) R1P_AUDIO_PORT (9904)
#        R1P_CONTROL_PORT (9905, TCP + UDP):
#        give a second container (another VARIANT/R1P_NAME/R1P_DIR/R1P_IMAGE)
#        its own ports, since containers share the host network
#        R1P_USER_IMAGES ($R1P_DIR/user-images): images for CreateInstance's
#        system_image/vendor_image/initramfs_image, mounted read-only at the
#        same path inside the container
#        R1P_INSTANCE_ROOT ($R1P_DIR/instances): where CreateInstance's
#        instance_dir may point, mounted read-write at the same path
#        R1P_ENV ("A=1 B=2"): extra container environment (e.g. VREND_DEBUG)
#        R1P_DRIVERS (cix-8180, under thirdparty/host-drivers)  VARIANT  MEM SMP USERDATA_MB
#        R1P_APT_MIRROR (replaces http://deb.debian.org in the image build;
#        default http://mirrors.ustc.edu.cn, empty for upstream)
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
KID="$(tr -d '[:space:]' < "$AOSP/KERNEL")"
R1P_HOST="${R1P_HOST:-user@10.10.2.142}"
R1P_DIR="${R1P_DIR:-/mmc/tebox}"
R1P_NAME="${R1P_NAME:-tebox-1}"
R1P_ADB_PORT="${R1P_ADB_PORT:-5555}"
R1P_GRPC_PORT="${R1P_GRPC_PORT:-50051}"
R1P_CAST_PORT="${R1P_CAST_PORT:-9901}"
R1P_AUDIO_PORT="${R1P_AUDIO_PORT:-9904}"
R1P_CONTROL_PORT="${R1P_CONTROL_PORT:-9905}"
R1P_IMAGE="${R1P_IMAGE:-tebox-r1p:latest}"
R1P_USER_IMAGES="${R1P_USER_IMAGES:-$R1P_DIR/user-images}"
R1P_INSTANCE_ROOT="${R1P_INSTANCE_ROOT:-$R1P_DIR/instances}"
R1P_DRIVERS="${R1P_DRIVERS:-cix-8180}"
R1P_BUILDER="${R1P_BUILDER:-tebox-r1p-builder}"
R1P_APT_MIRROR="${R1P_APT_MIRROR-http://mirrors.ustc.edu.cn}"
CONTEXT="$ROOT/out/r1p-context"
BIN="$ROOT/out/r1p-bin"

remote() {
  # sudo -A keeps stdin free for streamed docker images.
  ssh "$R1P_HOST" "export SUDO_ASKPASS=\${R1P_SUDO_ASKPASS:-\$HOME/.askpass}; $*"
}


build() {
  bash "$ROOT/scripts/fetch-host-drivers.sh" "$R1P_DRIVERS"
  rm -rf "$CONTEXT"
  mkdir -p "$CONTEXT"/{.ci,scripts,thirdparty,host-drivers,build/make/core,runtime-src/aosp/"$VARIANT"/qemu/cmdline}
  rsync -a --delete --exclude .git "$ROOT/qemu" "$CONTEXT/"
  rsync -a --delete --exclude .git "$ROOT/thirdparty/libepoxy" "$ROOT/thirdparty/virglrenderer" \
    "$ROOT/thirdparty/gfxstream" "$ROOT/thirdparty/rutabaga_gfx" "$CONTEXT/thirdparty/"
  mkdir -p "$CONTEXT/src/service"
  rsync -a --delete "$ROOT/src/service/cast" "$CONTEXT/src/service/"
  (cd "$ROOT/src/service/api" && GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build -o "$CONTEXT/tebox-api" ./cmd/tebox-api)
  rsync -a --delete "$ROOT/prebuilts/android-arm64/scrcpy/" "$CONTEXT/scrcpy/"
  cp "$ROOT"/.ci/{build-libepoxy.sh,build-virglrenderer.sh,build-qemu.sh,build-rutabaga.sh,requirements.txt} "$CONTEXT/.ci/"
  cp "$ROOT"/scripts/{env.sh,boot-qemu.sh,check-runtime-inputs.py,cix-mali-env.sh,cix-r1p-start.sh} \
    "$CONTEXT/scripts/"
  # scripts/env.sh sources this; host build scripts need it inside the context.
  cp "$ROOT/build/make/core/config.sh" "$CONTEXT/build/make/core/config.sh"
  cp "$ROOT/thirdparty/host-drivers/$R1P_DRIVERS"/*.deb "$CONTEXT/host-drivers/"
  cp "$AOSP/KERNEL" "$CONTEXT/runtime-src/aosp/$VARIANT/"
  [[ ! -f "$AOSP/GPU" ]] || cp "$AOSP/GPU" "$CONTEXT/runtime-src/aosp/$VARIANT/"
  cp "$AOSP/qemu/cmdline/boot" "$CONTEXT/runtime-src/aosp/$VARIANT/qemu/cmdline/"
  # boot-qemu.sh reads qemu/vulkan-hal at runtime to name the Vulkan HAL
  # (androidboot.hardware.vulkan); without it the default "ranchu" mismatches a
  # vendor.img that renamed vulkan.ranchu.so (e.g. OriginOS -> vulkan.qcom.so)
  # and ANGLE's Vulkan init fails, crash-looping SurfaceFlinger.
  [[ ! -f "$AOSP/qemu/vulkan-hal" ]] || cp "$AOSP/qemu/vulkan-hal" "$CONTEXT/runtime-src/aosp/$VARIANT/qemu/"
  # Bake GKI Image (not AOSP system/vendor/initramfs) so the container can
  # boot with only images/ mounted from the host.
  mkdir -p "$CONTEXT/runtime-src/kernel/$KID/gki"
  cp "$ROOT/src/kernel/$KID/gki/Image" "$CONTEXT/runtime-src/kernel/$KID/gki/Image"
  # R1P_QUICK=1: skip QEMU rebuild — layer kernel + variant metadata + fresh
  # tebox-api onto an existing runtime image (default: local $R1P_IMAGE).
  if [[ "${R1P_QUICK:-0}" == 1 ]]; then
    local base="${R1P_QUICK_FROM:-$R1P_IMAGE}"
    docker image inspect "$base" >/dev/null 2>&1 || {
      echo "missing base image $base (unset R1P_QUICK or docker pull/load it)" >&2
      exit 1
    }
    docker build --platform linux/arm64 -f "$ROOT/.ci/cix-r1p/Dockerfile.quick" \
      --build-arg BASE_IMAGE="$base" \
      --build-arg VARIANT="$VARIANT" --build-arg KERNEL_ID="$KID" \
      -t "$R1P_IMAGE" "$CONTEXT"
    return
  fi
  docker build --platform linux/arm64 -f "$ROOT/.ci/cix-r1p/Dockerfile" --build-arg APT_MIRROR="$R1P_APT_MIRROR" \
    --build-arg VARIANT="$VARIANT" --build-arg KERNEL_ID="$KID" -t "$R1P_IMAGE" "$CONTEXT"
  # Tag the compiler stage as an image so pruning the build cache keeps it.
  docker build --platform linux/arm64 -f "$ROOT/.ci/cix-r1p/Dockerfile" --target deps \
    --build-arg APT_MIRROR="$R1P_APT_MIRROR" -t "$R1P_BUILDER" "$CONTEXT"
}

bin() {
  docker image inspect "$R1P_BUILDER" >/dev/null 2>&1 || {
    echo "missing $R1P_BUILDER image; run: $0 build" >&2; exit 1; }
  mkdir -p "$BIN/service"
  (cd "$ROOT/src/service/api" && GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build -o "$BIN/tebox-api" ./cmd/tebox-api)
  # Objects stay in out/r1p-bin/service, so this is an incremental make.
  docker run --rm --platform linux/arm64 \
    -v "$ROOT/src/service/cast:/src/src/service/cast:ro" -v "$BIN/service:/src/out/service" \
    -w /src "$R1P_BUILDER" make -C src/service/cast CAST_SYSTEM_FFMPEG=1
  # VirGL (and its libepoxy, once) build incrementally under out/r1p-bin too.
  mkdir -p "$BIN/host" "$BIN/build"
  docker run --rm --platform linux/arm64 \
    -v "$ROOT/.ci:/src/.ci:ro" -v "$ROOT/scripts/env.sh:/src/scripts/env.sh:ro" \
    -v "$ROOT/thirdparty/libepoxy:/src/thirdparty/libepoxy:ro" \
    -v "$ROOT/thirdparty/virglrenderer:/src/thirdparty/virglrenderer:ro" \
    -v "$BIN/host:/src/prebuilts/host/linux-arm64" -v "$BIN/build:/src/out" \
    -w /src "$R1P_BUILDER" sh -c '
      [ -f prebuilts/host/linux-arm64/libepoxy/lib/pkgconfig/epoxy.pc ] || bash .ci/build-libepoxy.sh
      VIRGL_VIDEO=1 VIRGL_VIDEO_BACKEND=v4l2 bash .ci/build-virglrenderer.sh' >/dev/null
  cp -L "$BIN/host/virglrenderer/lib/libvirglrenderer.so.1" "$BIN/libvirglrenderer.so.1"
  remote "sudo -A mkdir -p $R1P_DIR/bin"
  local f name
  for f in "$BIN/tebox-api" "$BIN/service/tebox-cast" "$BIN/libvirglrenderer.so.1" \
           "$ROOT/scripts/boot-qemu.sh" "$ROOT/scripts/cix-r1p-start.sh"; do
    name="$(basename "$f")"
    gzip -1 -c "$f" | remote "gunzip | sudo -A tee $R1P_DIR/bin/$name.part >/dev/null \
      && sudo -A chmod 755 $R1P_DIR/bin/$name.part && sudo -A mv $R1P_DIR/bin/$name.part $R1P_DIR/bin/$name"
  done
  # cix-r1p-start.sh installs these over the image's cmdline / GPU file.
  remote "sudo -A tee $R1P_DIR/bin/cmdline-boot.$VARIANT.part >/dev/null \
    && sudo -A mv $R1P_DIR/bin/cmdline-boot.$VARIANT.part $R1P_DIR/bin/cmdline-boot.$VARIANT" < "$AOSP/qemu/cmdline/boot"
  if [[ -f "$AOSP/GPU" ]]; then
    remote "sudo -A tee $R1P_DIR/bin/GPU.$VARIANT.part >/dev/null \
      && sudo -A mv $R1P_DIR/bin/GPU.$VARIANT.part $R1P_DIR/bin/GPU.$VARIANT" < "$AOSP/GPU"
  fi
  # Recreating reboots the guests (tebox-api owns the QEMU processes) and
  # picks up the VirGL mount below.
  start >/dev/null
  echo "updated tebox-api, tebox-cast, VirGL, boot-qemu.sh and GPU on ${R1P_HOST#*@}; container $R1P_NAME restarted"
}

push() {
  # A new image supersedes binaries uploaded by `bin`.
  remote "sudo -A rm -rf $R1P_DIR/bin"
  docker save "$R1P_IMAGE" | gzip -1 | remote 'gunzip | sudo -A docker load'
}

images() {
  "$ROOT/scripts/ensure-guest-downloads.sh" "$VARIANT" >/dev/null 2>&1 || true
  bash "$ROOT/scripts/build-vendor-img.sh" "$VARIANT"
  bash "$ROOT/scripts/build-initramfs.sh" "$VARIANT"
  remote "sudo -A mkdir -p $R1P_DIR/images $R1P_DIR/$R1P_NAME"
  local f
  # Kernel is baked into the Docker image; only upload AOSP imgs here.
  # Set R1P_UPLOAD_KERNEL=1 to also push a host override under $R1P_DIR/kernel.
  local imgs=("$SYSTEM_IMG" "$VENDOR_IMG" "$INITRAMFS_IMG")
  [[ "${R1P_UPLOAD_KERNEL:-0}" == 1 ]] && imgs+=("$ROOT/src/kernel/$KID/gki/Image")
  for f in "${imgs[@]}"; do
    local dst="$R1P_DIR/images/$(basename "$f")"
    [[ "$(basename "$f")" == Image ]] && {
      remote "sudo -A mkdir -p $R1P_DIR/kernel"
      dst="$R1P_DIR/kernel/Image"
    }
    local sum
    sum="$(shasum -a 256 "$f" | cut -d' ' -f1)"
    if remote "sudo -A sha256sum $dst 2>/dev/null" | grep -q "^$sum "; then
      echo "up to date: $dst"
      continue
    fi
    echo "upload: $f -> $dst"
    gzip -1 -c "$f" | remote "gunzip | sudo -A tee $dst.part >/dev/null && sudo -A mv $dst.part $dst"
  done
}

start() {
  stop >/dev/null 2>&1 || true
  # A VirGL uploaded by `bin` replaces the image's library (docker resolves
  # the .so.1 symlink inside the image to the real file).
  local virgl=
  remote "sudo -A test -f $R1P_DIR/bin/libvirglrenderer.so.1" && \
    virgl="-v $R1P_DIR/bin/libvirglrenderer.so.1:/opt/tebox/prebuilts/host/linux-arm64/virglrenderer/lib/libvirglrenderer.so.1:ro"
  # boot-qemu.sh, tebox-api and tebox-cast are copied from the override dir
  # by cix-r1p-start.sh; mounting one over its target too makes that cp fail.
  remote "sudo -A test -f $R1P_DIR/bin/cix-r1p-start.sh" && \
    virgl+=" -v $R1P_DIR/bin/cix-r1p-start.sh:/opt/tebox/scripts/cix-r1p-start.sh:ro"
  # Optional host kernel override; otherwise use the Image baked into $R1P_IMAGE.
  local kernel_vol=
  if remote "sudo -A test -f $R1P_DIR/kernel/Image"; then
    kernel_vol="-v $R1P_DIR/kernel:/opt/tebox/src/kernel/$KID/gki:ro"
  fi
  remote "sudo -A mkdir -p $R1P_USER_IMAGES $R1P_INSTANCE_ROOT $R1P_DIR/$R1P_NAME"
  # Host networking lets QEMU's user-mode hostfwd publish adb on the LAN.
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
  echo "adb connect ${R1P_HOST#*@}:$R1P_ADB_PORT"
  echo "gRPC ${R1P_HOST#*@}:$R1P_GRPC_PORT  video ${R1P_HOST#*@}:$R1P_CAST_PORT  audio ${R1P_HOST#*@}:$R1P_AUDIO_PORT  control tcp+udp ${R1P_HOST#*@}:$R1P_CONTROL_PORT  instance $R1P_NAME"
}

stop() { remote "sudo -A docker rm -f $R1P_NAME"; }
status() { remote "sudo -A docker ps -a --filter name=$R1P_NAME"; }
logs() { remote "sudo -A docker logs --tail ${TAIL:-200} $R1P_NAME"; }

export_img() {
  mkdir -p "$ROOT/out"
  local stamp out
  stamp="$(date +%Y%m%d)"
  out="$ROOT/out/tebox-r1p-${VARIANT}-${stamp}.tar.gz"
  echo "exporting $R1P_IMAGE → $out"
  docker save "$R1P_IMAGE" | gzip -1 > "$out"
  ls -lh "$out"
  # Sanity: no AOSP guest imgs inside the archive layers.
  if docker run --rm --entrypoint sh "$R1P_IMAGE" -c \
      'find /opt/tebox -name "*.img" 2>/dev/null | grep -q .'; then
    echo "error: image contains *.img (AOSP must stay host-mounted)" >&2
    exit 1
  fi
  docker run --rm --entrypoint sh "$R1P_IMAGE" -c \
    "test -f /opt/tebox/src/kernel/$KID/gki/Image && ls -lh /opt/tebox/src/kernel/$KID/gki/Image"
  echo "wrote $out"
}

case "${1:-all}" in
  build|push|images|start|stop|status|logs|bin) "$1" ;;
  export) export_img ;;
  all) build; push; images; start ;;
  *) echo "usage: $0 [build|push|images|start|stop|status|logs|export|all|bin]" >&2; exit 2 ;;
esac
