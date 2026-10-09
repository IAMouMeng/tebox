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
# Android guest property: set vendor.tebox.rgb_row_alignment=64 for Mali.
