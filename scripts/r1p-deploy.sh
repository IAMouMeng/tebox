#!/usr/bin/env bash
# Build the CIX Sky1 (MYT R1P) runtime image locally and run it on a remote
# R1P over SSH. Only binaries and runtime images leave this machine. The
# container runs tebox-api (gRPC 50051, cast 9901, UDP input 9903).
#
# Usage: scripts/r1p-deploy.sh [build|push|images|start|stop|status|logs|all]
# Env:   R1P_HOST (user@10.10.2.142)  R1P_DIR (/mmc/tebox)  R1P_NAME (tebox-1)
#        R1P_ADB_PORT (5555)  R1P_SUDO_ASKPASS (~/.askpass on the remote)
#        CIX_DEBS (downloads/cix)  VARIANT  MEM SMP USERDATA_MB
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
source "$ROOT/scripts/env.sh"
KID="$(tr -d '[:space:]' < "$AOSP/KERNEL")"
R1P_HOST="${R1P_HOST:-user@10.10.2.142}"
R1P_DIR="${R1P_DIR:-/mmc/tebox}"
R1P_NAME="${R1P_NAME:-tebox-1}"
R1P_ADB_PORT="${R1P_ADB_PORT:-5555}"
R1P_IMAGE="${R1P_IMAGE:-tebox-r1p:latest}"
CIX_DEBS="${CIX_DEBS:-$ROOT/downloads/cix}"
CIX_REPO="https://radxa-repo.github.io/cd8180-bookworm-test/pool/main/c"
CONTEXT="$ROOT/out/r1p-context"

remote() {
  # sudo -A keeps stdin free for streamed docker images.
  ssh "$R1P_HOST" "export SUDO_ASKPASS=\${R1P_SUDO_ASKPASS:-\$HOME/.askpass}; $*"
}

fetch_debs() {
  mkdir -p "$CIX_DEBS"
  local deb
  for deb in cix-gpu-umd/cix-gpu-umd_2.0.0_arm64.deb cix-libglvnd/cix-libglvnd_1.7.0_arm64.deb; do
    [[ -f "$CIX_DEBS/$(basename "$deb")" ]] || curl -fL -o "$CIX_DEBS/$(basename "$deb")" "$CIX_REPO/$deb"
  done
}

build() {
  fetch_debs
  rm -rf "$CONTEXT"
  mkdir -p "$CONTEXT"/{.ci,scripts,thirdparty,cix-debs,runtime-src/aosp/"$VARIANT"/qemu/cmdline}
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
  cp "$CIX_DEBS"/cix-gpu-umd_*.deb "$CIX_DEBS"/cix-libglvnd_*.deb "$CONTEXT/cix-debs/"
  cp "$AOSP/KERNEL" "$CONTEXT/runtime-src/aosp/$VARIANT/"
  cp "$AOSP/qemu/cmdline/boot" "$CONTEXT/runtime-src/aosp/$VARIANT/qemu/cmdline/"
  docker build --platform linux/arm64 -f "$ROOT/.ci/cix-r1p/Dockerfile" \
    --build-arg VARIANT="$VARIANT" --build-arg KERNEL_ID="$KID" -t "$R1P_IMAGE" "$CONTEXT"
}

push() {
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
    $R1P_IMAGE"
  echo "adb connect ${R1P_HOST#*@}:$R1P_ADB_PORT"
  echo "gRPC ${R1P_HOST#*@}:50051  cast ${R1P_HOST#*@}:9901  input udp ${R1P_HOST#*@}:9903  instance $R1P_NAME"
}

stop() { remote "sudo -A docker rm -f $R1P_NAME"; }
status() { remote "sudo -A docker ps -a --filter name=$R1P_NAME"; }
logs() { remote "sudo -A docker logs --tail ${TAIL:-200} $R1P_NAME"; }

case "${1:-all}" in
  build|push|images|start|stop|status|logs) "$1" ;;
  all) build; push; images; start ;;
  *) echo "usage: $0 [build|push|images|start|stop|status|logs|all]" >&2; exit 2 ;;
esac
