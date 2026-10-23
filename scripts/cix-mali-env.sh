#!/usr/bin/env bash
# Source inside the CIX runtime container before launching tebox.
[[ "$(uname -s)" == Linux && "$(uname -m)" == aarch64 ]] || { echo "CIX Mali requires Linux ARM64" >&2; return 1; }
CIX_GPU_PREFIX="${CIX_GPU_PREFIX:-/opt/cixgpu-pro}"
CIX_GPU_COMPAT="${CIX_GPU_COMPAT:-/opt/cixgpu-compat}"
[[ -c /dev/dma_heap/system ]] || { echo "missing /dev/dma_heap/system" >&2; return 1; }
[[ -d "$CIX_GPU_PREFIX/lib/aarch64-linux-gnu" && -d "$CIX_GPU_COMPAT/share/glvnd/egl_vendor.d" ]] || {
  echo "missing CIX userspace driver or GLVND configuration" >&2; return 1;
}
export LD_LIBRARY_PATH="$CIX_GPU_PREFIX/lib/aarch64-linux-gnu${LD_LIBRARY_PATH:+:$LD_LIBRARY_PATH}"
export __EGL_VENDOR_LIBRARY_DIRS="$CIX_GPU_COMPAT/share/glvnd/egl_vendor.d"
export FORCE_VIRGL=1 QEMU_DISPLAY=egl-headless,gl=es
export TEBOX_EGL_ALLOW_NO_DMABUF_EXPORT=1
# Remaps GPU=gfxstream → mali in boot-qemu on this host. Override with
# TEBOX_MALI=0 to keep gfxstream/virgl from the GPU file.
export TEBOX_MALI="${TEBOX_MALI:-1}"
export TEBOX_V4L2="${TEBOX_V4L2:-1}"
export TEBOX_MALI_KICK_MS="${TEBOX_MALI_KICK_MS:-32}"
# Stable: free-mmap SAME_VA at ~0x5f. WIN_IMPORT off (cookie SEGV). Device 64KiB.
export TEBOX_MALI_ARENA="${TEBOX_MALI_ARENA:-0}"
export TEBOX_MALI_ARENA_BASE="${TEBOX_MALI_ARENA_BASE:-0x5f00000000}"
export TEBOX_MALI_WIN_BG_MB="${TEBOX_MALI_WIN_BG_MB:-1024}"
export TEBOX_MALI_HOSTMEM_BG="${TEBOX_MALI_HOSTMEM_BG:-1}"
export TEBOX_MALI_WIN_IMPORT="${TEBOX_MALI_WIN_IMPORT:-0}"
export TEBOX_MALI_DEVICE_MAX="${TEBOX_MALI_DEVICE_MAX:-65536}"
export TEBOX_MALI_WIN_MIN_PAGES="${TEBOX_MALI_WIN_MIN_PAGES:-1024}"
