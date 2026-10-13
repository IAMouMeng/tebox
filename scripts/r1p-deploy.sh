#!/usr/bin/env bash
# Build the CIX Sky1 (MYT R1P) runtime image locally and run it on a remote
# R1P over SSH. Only binaries and runtime images leave this machine. The
# container runs tebox-api (gRPC 50051, cast 9901, UDP input 9903).
#
# Usage: scripts/r1p-deploy.sh [build|push|images|start|stop|status|logs|all|bin]
#   bin: rebuild only tebox-api and tebox-cast, upload them to $R1P_DIR/bin
#        (mounted over the image's copies) and restart the container. Needs
#        the tebox-r1p-builder image that `build` tags; no image transfer.
# Env:   R1P_HOST (user@10.10.2.142)  R1P_DIR (/mmc/tebox)  R1P_NAME (tebox-1)
#        R1P_ADB_PORT (5555)  R1P_SUDO_ASKPASS (~/.askpass on the remote)
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
R1P_IMAGE="${R1P_IMAGE:-tebox-r1p:latest}"
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
  mkdir -p "$CONTEXT"/{.ci,scripts,thirdparty,host-drivers,runtime-src/aosp/"$VARIANT"/qemu/cmdline}
  rsync -a --delete --exclude .git "$ROOT/qemu" "$CONTEXT/"
  rsync -a --delete --exclude .git "$ROOT/thirdparty/libepoxy" "$ROOT/thirdparty/virglrenderer" \
    "$CONTEXT/thirdparty/"
  mkdir -p "$CONTEXT/src/service"
  rsync -a --delete "$ROOT/src/service/cast" "$CONTEXT/src/service/"
  (cd "$ROOT/src/service/api" && GOOS=linux GOARCH=arm64 CGO_ENABLED=0 go build -o "$CONTEXT/tebox-api" ./cmd/tebox-api)
  rsync -a --delete "$ROOT/prebuilts/android-arm64/scrcpy/" "$CONTEXT/scrcpy/"
  cp "$ROOT"/.ci/{build-libepoxy.sh,build-virglrenderer.sh,build-qemu.sh,requirements.txt} "$CONTEXT/.ci/"
  cp "$ROOT"/scripts/{env.sh,boot-qemu.sh,check-runtime-inputs.py,cix-mali-env.sh,cix-r1p-start.sh} \
    "$CONTEXT/scripts/"
  cp "$ROOT/thirdparty/host-drivers/$R1P_DRIVERS"/*.deb "$CONTEXT/host-drivers/"
  cp "$AOSP/KERNEL" "$CONTEXT/runtime-src/aosp/$VARIANT/"
  cp "$AOSP/qemu/cmdline/boot" "$CONTEXT/runtime-src/aosp/$VARIANT/qemu/cmdline/"
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
  remote "sudo -A mkdir -p $R1P_DIR/bin"
  local f name
  for f in "$BIN/tebox-api" "$BIN/service/tebox-cast"; do
    name="$(basename "$f")"
    gzip -1 -c "$f" | remote "gunzip | sudo -A tee $R1P_DIR/bin/$name.part >/dev/null \
      && sudo -A chmod 755 $R1P_DIR/bin/$name.part && sudo -A mv $R1P_DIR/bin/$name.part $R1P_DIR/bin/$name"
  done
  # Restarting reboots the guests: tebox-api owns the QEMU processes.
  remote "sudo -A docker restart $R1P_NAME" >/dev/null
  echo "updated tebox-api and tebox-cast on ${R1P_HOST#*@}; container $R1P_NAME restarted"
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
  remote "sudo -A mkdir -p $R1P_DIR/images $R1P_DIR/kernel $R1P_DIR/$R1P_NAME"
  local f
  for f in "$SYSTEM_IMG" "$VENDOR_IMG" "$INITRAMFS_IMG" "$ROOT/src/kernel/$KID/gki/Image"; do
    local dst="$R1P_DIR/images/$(basename "$f")"
    [[ "$(basename "$f")" == Image ]] && dst="$R1P_DIR/kernel/Image"
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
  # Host networking lets QEMU's user-mode hostfwd publish adb on the LAN.
  remote "sudo -A docker run -d --name $R1P_NAME --restart unless-stopped --network host \
    --device /dev/kvm --device /dev/mali0 --device /dev/dri \
    --device /dev/dma_heap/system --device /dev/dma_heap/system-uncached \
    --device /dev/video1 \
    -e ADB_PORT=$R1P_ADB_PORT -e MEM=${MEM:-4096} -e SMP=${SMP:-4} \
    -e USERDATA_MB=${USERDATA_MB:-8192} -e R1P_INSTANCE=$R1P_NAME \
    -v $R1P_DIR/images:/opt/tebox/src/aosp/$VARIANT/images:ro \
    -v $R1P_DIR/kernel:/opt/tebox/src/kernel/$KID/gki:ro \
    -v $R1P_DIR/$R1P_NAME:/data \
    -v $R1P_DIR/bin:/opt/tebox/override:ro \
    $R1P_IMAGE"
  echo "adb connect ${R1P_HOST#*@}:$R1P_ADB_PORT"
  echo "gRPC ${R1P_HOST#*@}:50051  cast ${R1P_HOST#*@}:9901  input udp ${R1P_HOST#*@}:9903  instance $R1P_NAME"
}

stop() { remote "sudo -A docker rm -f $R1P_NAME"; }
status() { remote "sudo -A docker ps -a --filter name=$R1P_NAME"; }
logs() { remote "sudo -A docker logs --tail ${TAIL:-200} $R1P_NAME"; }

case "${1:-all}" in
  build|push|images|start|stop|status|logs|bin) "$1" ;;
  all) build; push; images; start ;;
  *) echo "usage: $0 [build|push|images|start|stop|status|logs|all|bin]" >&2; exit 2 ;;
esac
