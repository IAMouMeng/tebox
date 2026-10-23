# Host / NDK / product paths. Sourced by build/envsetup.sh after lunch.
# QEMU launch scripts may source this via scripts/env.sh.

if [[ -z "${ROOT:-}" ]]; then
  if [[ -n "${BASH_SOURCE[0]:-}" ]]; then
    _cfg_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
  elif [[ -n "${ZSH_VERSION:-}" ]]; then
    _cfg_dir="$(cd "$(dirname "${(%):-%x}")" && pwd)"
  else
    _cfg_dir="$(cd "$(dirname "$0")" && pwd)"
  fi
  ROOT="$(cd "$_cfg_dir/../../.." && pwd)"
  unset _cfg_dir
fi

VARIANT="${TARGET_PRODUCT:-${VARIANT:-aosp_arm64-BP4A.251205.006}}"
TARGET_PRODUCT="${TARGET_PRODUCT:-$VARIANT}"
TARGET_BUILD_VARIANT="${TARGET_BUILD_VARIANT:-userdebug}"
TARGET_ARCH="${TARGET_ARCH:-arm64}"
API="${API:-34}"

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
RUTABAGA_PREFIX="$HOST_PRE/rutabaga"
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
TARGET_OUT_VENDOR="${TARGET_OUT_VENDOR:-$VENDOR_STUB}"
BUSYBOX="$AOSP/qemu/busybox"
INIT_DIR="$AOSP/qemu/init"
GSI_LIBS="$AOSP/prebuilts/gsi-lib64"
ANDROID_HEADERS="$ROOT/thirdparty/android-headers/include"
NDK="${NDK:-${ANDROID_NDK:-$ROOT/toolchains/android-ndk}}"
ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-$ROOT/toolchains/android-sdk}}"
AIDL="${AIDL:-$ANDROID_SDK_ROOT/build-tools/36.1.0/aidl}"
GKI_ROOT="$ROOT"
GKI_HARDWARE="$ROOT/src/common/hardware/interfaces"
GKI_HAL_SHIM="$ROOT/src/common/hardware/shim"
GKI_HAL_VARIANT="$HARDWARE"
GKI_OUT="$ROOT/out"
HI="$ROOT/thirdparty/hardware-interfaces"
OUT_DIR="$ROOT/out"
ANDROID_NDK="$NDK"

if [[ "$HOST_OS" == darwin ]]; then
  E2="${E2:-$(brew --prefix e2fsprogs)/sbin}"
else
  E2="${E2:-/usr/sbin}"
fi

export ROOT VARIANT TARGET_PRODUCT TARGET_BUILD_VARIANT TARGET_ARCH API
export HOST_OS HOST_ARCH HOST_ID HOST_PRE ANDROID_PRE
export NDK NDK_HOST_TAG ANDROID_NDK ANDROID_SDK_ROOT AIDL ANDROID_HEADERS
export AOSP IMAGES HARDWARE SYSTEM_IMG VENDOR_IMG INITRAMFS_IMG
export VENDOR_STUB TARGET_OUT_VENDOR BUSYBOX INIT_DIR GSI_LIBS
export GKI_ROOT GKI_HARDWARE GKI_HAL_SHIM GKI_HAL_VARIANT GKI_OUT HI OUT_DIR
export EPOXY_PREFIX VIRGL_PREFIX RUTABAGA_PREFIX DRM_PREFIX MESA_PREFIX MESA_PRE
export E2
