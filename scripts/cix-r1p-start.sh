#!/usr/bin/env bash
# Container entrypoint for CIX Sky1 hosts (MYT R1P); see .ci/cix-r1p/Dockerfile.
# Expects /dev/kvm, /dev/mali0, /dev/dma_heap/*, /dev/dri and the V4L2
# encoder, plus mounts:
#   $TEBOX/src/aosp/$VARIANT/images  system.img vendor.img initramfs.img (ro)
#   $TEBOX/src/kernel/$KERNEL_ID/gki Image (ro)
#   /data                            instances (userdata, QMP, cast sockets)
#
# tebox-api manages the instances and serves gRPC (50051), cast (9901) and
# UDP input (9903) on the LAN. R1P_INSTANCE is created on first start and
# started automatically; TEBOX_DIRECT=1 boots it with boot-qemu.sh alone.
set -euo pipefail
TEBOX="${TEBOX:-/opt/tebox}"
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
  --input-udp "${TEBOX_INPUT_UDP_ADDR:-0.0.0.0:9903}" \
  --start "${TEBOX_AUTOSTART:-$ID}"
