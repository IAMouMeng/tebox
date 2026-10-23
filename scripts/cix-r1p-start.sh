#!/usr/bin/env bash
# Container entrypoint for CIX Sky1 hosts (MYT R1P); see .ci/cix-r1p/Dockerfile.
# Expects /dev/kvm, /dev/mali0, /dev/dma_heap/*, /dev/dri and the V4L2
# encoder, plus mounts:
#   $TEBOX/src/aosp/$VARIANT/images  system.img vendor.img initramfs.img (ro)
#   /data                            instances (userdata, QMP, cast sockets)
# Kernel Image is baked into the image at src/kernel/$KERNEL_ID/gki/; an
# optional host bind-mount of that path can override it.
#
# tebox-api manages the instances and serves gRPC (50051), video (9901),
# audio (9904) and control (9905, TCP + UDP input) on the LAN. R1P_INSTANCE is created on first start and
# started automatically; TEBOX_DIRECT=1 boots it with boot-qemu.sh alone.
set -euo pipefail
TEBOX="${TEBOX:-/opt/tebox}"
# Binaries uploaded by `r1p-deploy.sh bin` (mounted at override/) replace the
# ones baked into the image, so API, cast, boot script and QEMU updates skip
# an image rebuild.
for f in bin/tebox-api out/service/tebox-cast scripts/boot-qemu.sh scripts/cix-mali-env.sh; do
  [[ -e "$TEBOX/override/${f##*/}" ]] && cp "$TEBOX/override/${f##*/}" "$TEBOX/$f"
done
if [[ -x "$TEBOX/override/boot-qemu.sh.${VARIANT:?}" ]]; then
  cp "$TEBOX/override/boot-qemu.sh.$VARIANT" "$TEBOX/scripts/boot-qemu.sh"
fi
# Keyed by variant: containers of different variants share override/.
if [[ -f "$TEBOX/override/cmdline-boot.${VARIANT:?}" ]]; then
  cp "$TEBOX/override/cmdline-boot.$VARIANT" "$TEBOX/src/aosp/$VARIANT/qemu/cmdline/boot"
fi
# GPU file (gfxstream|virgl): same override pattern as cmdline.
if [[ -f "$TEBOX/override/GPU.${VARIANT:?}" ]]; then
  mkdir -p "$TEBOX/src/aosp/$VARIANT"
  cp "$TEBOX/override/GPU.$VARIANT" "$TEBOX/src/aosp/$VARIANT/GPU"
fi
for q in "qemu-system-aarch64.$VARIANT" qemu-system-aarch64; do
  if [[ -x "$TEBOX/override/$q" ]]; then
    mkdir -p "$TEBOX/prebuilts/host/linux-arm64/qemu/bin"
    cp "$TEBOX/override/$q" "$TEBOX/prebuilts/host/linux-arm64/qemu/bin/qemu-system-aarch64"
    break
  fi
done
source "$TEBOX/scripts/cix-mali-env.sh"
export LD_LIBRARY_PATH="$LD_LIBRARY_PATH:$CIX_GPU_COMPAT/lib/aarch64-linux-gnu:$TEBOX/lib"
export ADB_BIND="${ADB_BIND:-0.0.0.0}"
# Mali GBM already pads linear pitches; the padded-RGB row-alignment hint
# (androidboot.tebox.rgb_row_alignment) skews CPU-written buffers here.
export QEMU_EXTRA_APPEND="androidboot.tebox.video_encode=1${QEMU_EXTRA_APPEND:+ $QEMU_EXTRA_APPEND}"

if [[ "${TEBOX_DIRECT:-0}" == 1 ]]; then
  export TEBOX_INSTANCE_DIR="${TEBOX_INSTANCE_DIR:-/data}"
  export ADB_PORT="${ADB_PORT:-5555}" QMP_SOCKET="${QMP_SOCKET:-$TEBOX_INSTANCE_DIR/qmp.sock}"
  export MEM="${MEM:-4096}" SMP="${SMP:-4}" USERDATA_MB="${USERDATA_MB:-8192}"
  rm -f "$QMP_SOCKET"
  exec "$TEBOX/scripts/boot-qemu.sh" "${VARIANT:?}"
fi

ID="${R1P_INSTANCE:-tebox-1}"
DIR="/data/instances/$ID"
if [[ ! -f "$DIR/instance.json" ]]; then
  mkdir -p "$DIR"
  # Keep the userdata of the earlier single-instance layout (/data/userdata.img).
  if [[ -f /data/userdata.img ]]; then
    mv /data/userdata.img "$DIR/userdata.img"
  fi
  printf '{"id":"%s","name":"%s","variant":"%s","adbPort":%d,"cpuCores":%d,"memoryMb":%d,"diskMb":%d}\n' \
    "$ID" "$ID" "${VARIANT:?}" "${ADB_PORT:-5555}" "${SMP:-4}" "${MEM:-4096}" "${USERDATA_MB:-8192}" \
    > "$DIR/instance.json"
fi

export TEBOX_ROOT="$TEBOX" TEBOX_DATA_DIR=/data TEBOX_VARIANT="$VARIANT"
exec "$TEBOX/bin/tebox-api" \
  --listen "${TEBOX_GRPC_ADDR:-0.0.0.0:50051}" \
  --cast-listen "${TEBOX_CAST_ADDR:-0.0.0.0:9901}" \
  --audio-listen "${TEBOX_AUDIO_ADDR:-0.0.0.0:9904}" \
  --control-listen "${TEBOX_CONTROL_ADDR:-0.0.0.0:9905}" \
  --start "${TEBOX_AUTOSTART:-$ID}"
