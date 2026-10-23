#!/usr/bin/env bash
# NDK recipe for cc_binary. Product config lives in device.mk / Android.bp.
set -euo pipefail

MODULE=${MODULE:?}
OUT=${OUT:?}
PATH_DIR=${PATH_DIR:-}
AIDL_SPECS=${AIDL_SPECS:-}
SRCS=${SRCS:-}
INCLUDES=${INCLUDES:-}
LIBS=${LIBS:-}
LDFLAGS=${LDFLAGS:-}
FORBIDDEN=${FORBIDDEN:-}

: "${GKI_ROOT:?}" "${VARIANT:?}" "${HI:?}" "${GSI_LIBS:?}" "${GKI_HAL_SHIM:?}"
: "${ANDROID_HEADERS:?}" "${AIDL:?}" "${CXX:?}" "${API:?}"

resolve_aidl() {
  local spec=$1
  local name=${spec%:*}
  local ver=${spec##*:}
  local aidl_api="" other="" d
  while IFS= read -r d; do
    if [[ "$d" == *"/aidl_api/$name/$ver" ]]; then
      aidl_api=$d
      break
    fi
    other=${other:-$d}
  done < <(find "$HI" -type d \( -path "*/aidl_api/$name/$ver" -o -path "*/$name/$ver" \) 2>/dev/null | sort)
  local found=${aidl_api:-$other}
  [[ -n "$found" && -d "$found" ]] || { echo "missing frozen AIDL $spec under $HI" >&2; return 1; }
  printf '%s\n' "$found"
}

gen="$OUT/aidl"
rm -rf "$gen" "$OUT/obj"
mkdir -p "$gen/src" "$gen/include" "$OUT/obj" "$OUT/bin"

aidl_dirs=()
aidl_includes=()
# shellcheck disable=SC2086
for spec in ${AIDL_SPECS}; do
  dir=$(resolve_aidl "$spec")
  aidl_dirs+=("$dir")
  aidl_includes+=(-I "$dir")
done

hash=""
for dir in "${aidl_dirs[@]+"${aidl_dirs[@]}"}"; do
  ver=$(basename "$dir")
  [[ -f "$dir/.hash" ]] || { echo "missing $dir/.hash" >&2; exit 1; }
  hash=$(head -n1 "$dir/.hash")
  sources=()
  while IFS= read -r src; do sources+=("$src"); done < <(find "$dir" -name '*.aidl' | sort)
  [[ ${#sources[@]} -gt 0 ]] || { echo "no .aidl in $dir" >&2; exit 1; }
  "$AIDL" --lang=ndk --structured --stability=vintf --min_sdk_version="$API" \
    --version="$ver" --hash="$hash" \
    -o "$gen/src" -h "$gen/include" "${aidl_includes[@]}" "${sources[@]}"
done

if [[ -n "$FORBIDDEN" ]] && grep -q "$FORBIDDEN" "$gen/include"/aidl/*/*/*/*.h 2>/dev/null; then
  echo "ERROR: generated AIDL contains forbidden symbol $FORBIDDEN" >&2
  exit 1
fi

cflags=(
  -std=c++20 -O2 -fPIC -DBINDER_STABILITY_SUPPORT
  -Wno-unused-parameter -Wno-unused-variable -Wno-unused-but-set-variable
  -Wno-macro-redefined -Wno-gcc-compat -Wno-deprecated-declarations
  -I"$gen/include" -I"$GKI_HAL_SHIM" -I"$ANDROID_HEADERS"
)
[[ -n "$PATH_DIR" ]] && cflags+=(-I"$PATH_DIR")
# shellcheck disable=SC2086
for dir in ${INCLUDES}; do
  cflags+=(-I"$dir")
done
if [[ "${1:-}" == -- ]]; then
  shift
fi
cflags+=("$@")

ldlibs=()
# shellcheck disable=SC2086
for lib in ${LIBS}; do
  case $lib in
    lib*) ldlibs+=(-l"${lib#lib}") ;;
    *) ldlibs+=(-l"$lib") ;;
  esac
done
ldflags=()
# shellcheck disable=SC2086
for flag in ${LDFLAGS}; do
  ldflags+=("$flag")
done

objects=()
i=0
compile_one() {
  local src=$1
  i=$((i + 1))
  local obj="$OUT/obj/o${i}_$(basename "$src" | tr '.' '_').o"
  mkdir -p "$(dirname "$obj")"
  echo "  CC $(basename "$src")"
  "$CXX" -c "$src" -o "$obj" "${cflags[@]}"
  objects+=("$obj")
}

if [[ -d "$gen/src" ]]; then
  while IFS= read -r src; do compile_one "$src"; done < <(find "$gen/src" -name '*.cpp' | sort)
fi
# shellcheck disable=SC2086
for src in ${SRCS}; do
  compile_one "$src"
done

"$CXX" -o "$OUT/bin/$MODULE" "${objects[@]}" -L"$GSI_LIBS" "${ldflags[@]}" "${ldlibs[@]}"
echo "Built $OUT/bin/$MODULE (${MODULE}${hash:+ hash=$hash})"
