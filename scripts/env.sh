#!/usr/bin/env bash
# Shared project paths. Source from Bash build/launch scripts.
# Compatible with `source` from bash or zsh.
if [[ -n "${BASH_SOURCE[0]:-}" ]]; then
  _GKI_ENV_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
elif [[ -n "${ZSH_VERSION:-}" ]]; then
  _GKI_ENV_DIR="$(cd "$(dirname "${(%):-%x}")" && pwd)"
else
  _GKI_ENV_DIR="$(cd "$(dirname "$0")" && pwd)"
fi
ROOT="$(cd "$_GKI_ENV_DIR/.." && pwd)"
unset _GKI_ENV_DIR
VARIANT="${VARIANT:-aosp_arm64-BP4A.251205.006}"
case "$(uname -s)" in
  Darwin) HOST_OS=darwin; NDK_HOST_TAG=darwin-x86_64 ;;
  Linux) HOST_OS=linux; NDK_HOST_TAG=linux-x86_64 ;;
  *) echo "unsupported host OS: $(uname -s)" >&2; return 1 ;;
esac
case "$(uname -m)" in
  arm64|aarch64) HOST_ARCH=arm64 ;;
  x86_64|amd64) HOST_ARCH=x86_64 ;;
  *) echo "unsupported host architecture: $(uname -m)" >&2; return 1 ;;
esac
HOST_ID="$HOST_OS-$HOST_ARCH"
HOST_PRE="$ROOT/prebuilts/host/$HOST_ID"
ANDROID_PRE="$ROOT/prebuilts/android-arm64"
EPOXY_PREFIX="$HOST_PRE/libepoxy"
VIRGL_PREFIX="$HOST_PRE/virglrenderer"
DRM_PREFIX="$ANDROID_PRE/libdrm"
MESA_PREFIX="$ANDROID_PRE/mesa-install"
MESA_PRE="$ANDROID_PRE/mesa"
AOSP="$ROOT/src/aosp/$VARIANT"
IMAGES="$AOSP/images"
HARDWARE="$AOSP/hardware"
SYSTEM_IMG="$IMAGES/system.img"
VENDOR_IMG="$IMAGES/vendor.img"
INITRAMFS_IMG="$IMAGES/initramfs.img"
VENDOR_STUB="$AOSP/qemu/vendor"
BUSYBOX="$AOSP/qemu/busybox"
INIT_DIR="$AOSP/qemu/init"
GSI_LIBS="$AOSP/prebuilts/gsi-lib64"
ANDROID_HEADERS="$ROOT/thirdparty/android-headers/include"
NDK="${NDK:-${ANDROID_NDK:-$ROOT/toolchains/android-ndk}}"
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$ROOT/toolchains/android-sdk}}"
AIDL="${AIDL:-$ANDROID_SDK_ROOT/build-tools/36.1.0/aidl}"
export ANDROID_NDK="$NDK" ANDROID_SDK_ROOT
export NDK_HOST_TAG
if [[ "$HOST_OS" == darwin ]]; then
  E2="${E2:-$(brew --prefix e2fsprogs)/sbin}"
else
  E2="${E2:-/usr/sbin}"
fi
export E2
