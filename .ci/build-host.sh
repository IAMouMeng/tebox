#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
source scripts/env.sh
[[ "$HOST_ARCH" == arm64 ]] || { echo "Expected an ARM64 host, got $HOST_ID" >&2; exit 1; }
bash scripts/build-libepoxy.sh
bash scripts/build-virglrenderer.sh
QEMU_INSTALL=1 bash scripts/build-qemu.sh
"$HOST_PRE/qemu/bin/qemu-system-aarch64" --version
"$HOST_PRE/qemu/bin/qemu-system-aarch64" -accel help
python3 .ci/package-host.py
