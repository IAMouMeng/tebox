# Shared env for HAL gki_custom build/install scripts.
# Source after HERE=/SCRIPT_DIR=/LOCAL_PATH= is set. Requires VARIANT.
# Uses build/make/core/config.sh (not scripts/).

_hal_env_dir="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
_d="$_hal_env_dir"
while [[ "$_d" != "/" && ! -f "$_d/build/envsetup.sh" ]]; do
  _d="$(dirname "$_d")"
done
ROOT="${ROOT:-${GKI_ROOT:-${ANDROID_BUILD_TOP:-$_d}}}"
if [[ ! -f "$ROOT/build/make/core/config.sh" ]]; then
  echo "hal-env: cannot find tree root (build/envsetup.sh) from $_hal_env_dir" >&2
  return 1 2>/dev/null || exit 1
fi
if [[ -z "${VARIANT:-${TARGET_PRODUCT:-}}" ]]; then
  echo "hal-env: VARIANT is required" >&2
  return 1 2>/dev/null || exit 1
fi
VARIANT="${VARIANT:-$TARGET_PRODUCT}"
TARGET_PRODUCT="${TARGET_PRODUCT:-$VARIANT}"
# shellcheck source=/dev/null
source "$ROOT/build/make/core/config.sh"
AOSP="$ROOT/src/aosp/$VARIANT"
STUB="${TARGET_OUT_VENDOR:-$AOSP/qemu/vendor}"
GKI_VENDOR="${GKI_VENDOR:-$STUB}"
SHIM="${GKI_HAL_SHIM:-$ROOT/src/common/hardware/shim}"
GSI_LIBS="${GSI_LIBS:-$AOSP/prebuilts/gsi-lib64}"
unset _hal_env_dir _d
