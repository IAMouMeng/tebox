# AOSP-style envsetup. Must be sourced from the tree root (or any subdir):
#   source build/envsetup.sh
#   lunch
#   m

# Reject being executed.
if [[ -z "${ZSH_VERSION:-}" ]]; then
  if [[ -n "${BASH_SOURCE[0]:-}" && "${BASH_SOURCE[0]}" == "$0" ]]; then
    echo "Must be sourced: source build/envsetup.sh" >&2
    exit 1
  fi
fi

if [[ -n "${ZSH_VERSION:-}" ]]; then
  setopt KSH_ARRAYS 2>/dev/null || true
  setopt SH_WORD_SPLIT 2>/dev/null || true
fi

gettop() {
  local d here
  here="$(pwd)"
  d="$here"
  while [[ "$d" != "/" ]]; do
    if [[ -f "$d/build/envsetup.sh" && -d "$d/src/aosp" ]]; then
      printf '%s\n' "$d"
      return 0
    fi
    d="$(dirname "$d")"
  done
  if [[ -n "${ANDROID_BUILD_TOP:-}" && -f "$ANDROID_BUILD_TOP/build/envsetup.sh" ]]; then
    printf '%s\n' "$ANDROID_BUILD_TOP"
    return 0
  fi
  echo "Unable to locate the top of the tree. Try sourcing build/envsetup.sh from the repo root." >&2
  return 1
}

croot() {
  local T
  T="$(gettop)" || return 1
  cd "$T"
}

_lunch_products() {
  local T d name
  T="$(gettop)" || return 1
  for d in "$T"/src/aosp/*/; do
    [[ -d "$d" ]] || continue
    name="$(basename "$d")"
    [[ -f "$d/KERNEL" ]] || continue
    [[ -d "$d/images" || -d "$d/qemu" ]] || continue
    printf '%s\n' "$name"
  done | LC_ALL=C sort
}

_lunch_product() {
  local pick="$1"
  pick="${pick%/}"
  pick="${pick##*/}"
  case "$pick" in
    *-userdebug) printf '%s\n' "${pick%-userdebug}" ;;
    *-user) printf '%s\n' "${pick%-user}" ;;
    *-eng) printf '%s\n' "${pick%-eng}" ;;
    *) printf '%s\n' "$pick" ;;
  esac
}

printconfig() {
  echo "============================================"
  echo "PLATFORM_VERSION=16"
  echo "TARGET_PRODUCT=${TARGET_PRODUCT:-}"
  echo "TARGET_BUILD_VARIANT=${TARGET_BUILD_VARIANT:-}"
  echo "TARGET_ARCH=${TARGET_ARCH:-arm64}"
  echo "HOST_OS=${HOST_OS:-}"
  echo "HOST_ARCH=${HOST_ARCH:-}"
  echo "OUT_DIR=${OUT_DIR:-out}"
  echo "============================================"
}

lunch() {
  local T products pick idx i name default_idx default_name kid gpu mark
  T="$(gettop)" || return 1
  products=()
  while IFS= read -r name; do
    [[ -n "$name" ]] && products+=("$name")
  done <<EOF
$(_lunch_products)
EOF
  if ((${#products[@]} == 0)); then
    echo "no products under src/aosp/*/ (need KERNEL + images|qemu)" >&2
    return 1
  fi

  default_idx=1
  default_name=""
  i=1
  for name in "${products[@]}"; do
    if [[ "$name" == "aosp_arm64-BP4A.251205.006" || "$name" == "aosp_arm64" ]]; then
      default_idx=$i
      default_name="$name"
    fi
    i=$((i + 1))
  done
  [[ -n "$default_name" ]] || default_name="${products[0]}"

  pick="${1:-}"
  if [[ -z "$pick" ]]; then
    echo
    echo "You're building on $(uname -s)/$(uname -m)"
    echo
    echo "Lunch menu... pick a product:"
    echo
    i=1
    for name in "${products[@]}"; do
      kid="$(tr -d '[:space:]' < "$T/src/aosp/$name/KERNEL" 2>/dev/null || echo '?')"
      gpu="$(tr -d '[:space:]' < "$T/src/aosp/$name/GPU" 2>/dev/null || echo gfxstream)"
      mark=
      (( i == default_idx )) && mark=" *"
      printf '     %d. %s%s\n         kernel: %s  gpu: %s\n' "$i" "$name" "$mark" "$kid" "$gpu"
      i=$((i + 1))
    done
    echo
    if [[ ! -t 0 ]]; then
      idx=$default_idx
      echo "Which would you like? [$default_name] $default_name"
    else
      printf "Which would you like? [%s] " "$default_name"
      read -r idx || true
      [[ -z "${idx:-}" ]] && idx=$default_idx
    fi
    pick="$idx"
  fi

  if [[ "$pick" =~ ^[0-9]+$ ]]; then
    if (( pick < 1 || pick > ${#products[@]} )); then
      echo "invalid selection: $pick" >&2
      return 1
    fi
    pick="${products[$((pick - 1))]}"
  fi

  name="$(_lunch_product "$pick")"
  local ok=0
  for p in "${products[@]}"; do
    [[ "$p" == "$name" ]] && ok=1 && break
  done
  if (( ok == 0 )); then
    echo "unknown product: $pick" >&2
    echo "available:" >&2
    for p in "${products[@]}"; do
      printf '  %s\n' "$p"
    done
    return 1
  fi

  export ANDROID_BUILD_TOP="$T"
  export TOP="$T"
  export TARGET_PRODUCT="$name"
  export TARGET_BUILD_VARIANT=userdebug
  export VARIANT="$name"
  # shellcheck source=/dev/null
  source "$T/build/make/core/config.sh"
  echo
  printconfig
}

m() {
  local T args a
  T="$(gettop)" || return 1
  if [[ -z "${TARGET_PRODUCT:-}" ]]; then
    echo "Please lunch a combo first" >&2
    return 1
  fi
  args=()
  for a in "$@"; do
    case "$a" in
      -j*) ;;
      modules|--list) args+=(--list) ;;
      *) args+=("$a") ;;
    esac
  done
  python3 "$T/build/soong/soong.py" --variant "$TARGET_PRODUCT" "${args[@]+"${args[@]}"}"
}

mm() { m "$@"; }

hmm() {
  cat <<'EOF'
Functions:

  lunch [<product>|<n>] Choose a product (src/aosp/<product>/ folder name).
  m [<module>...]       Build the lunch'd product (Soong-lite HALs).
  mm                    Same as m.
  printconfig           Show TARGET_PRODUCT and toolchain paths.
  croot                 cd to the tree root.
  gettop                Print the tree root.
  hmm                   This help.

Typical:

  source build/envsetup.sh
  lunch aosp_arm64-BP4A.251205.006
  m
  m android.hardware.lights-service
EOF
}

export ANDROID_BUILD_TOP
ANDROID_BUILD_TOP="$(gettop 2>/dev/null || pwd)"
export TOP="$ANDROID_BUILD_TOP"

echo "including ${ANDROID_BUILD_TOP}/build/envsetup.sh ..."
echo "Run 'hmm' for help, 'lunch' to pick a product, 'm' to build."
