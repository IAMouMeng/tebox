#!/usr/bin/env bash
# Ensure runtime guest inputs exist locally (not tracked in Git):
#   src/aosp/<variant>/images/system.img
#   src/kernel/<id>/gki/Image
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
VARIANT="${1:?usage: ensure-guest-downloads.sh <variant>}"
AOSP="$ROOT/src/aosp/$VARIANT"
SYSTEM="$AOSP/images/system.img"

[[ -d "$AOSP" ]] || { echo "missing variant: $AOSP" >&2; exit 1; }
[[ -f "$AOSP/KERNEL" ]] || { echo "missing $AOSP/KERNEL" >&2; exit 1; }

KID="$(tr -d '[:space:]' < "$AOSP/KERNEL")"
KERNEL_IMAGE="$ROOT/src/kernel/$KID/gki/Image"

read -r LOCK_VARIANT SYSTEM_URL SYSTEM_SHA256 KERNEL_URL KERNEL_SHA256 < <(
  python3 - "$ROOT/.ci/guest.lock.json" "$KID" <<'PY'
import json
import sys

lock = json.loads(open(sys.argv[1]).read())
kid = sys.argv[2]
system = lock.get("system", {})
kernel = lock.get("kernel", {})
print(
    lock.get("variant", ""),
    system.get("url", ""),
    system.get("sha256", ""),
    kernel.get("url", ""),
    kernel.get("sha256", ""),
)
if lock.get("kernel_id", "") and lock["kernel_id"] != kid:
    raise SystemExit(f"variant KERNEL={kid} does not match guest.lock kernel_id={lock['kernel_id']}")
PY
)

verify_sha256() {
  local file=$1 expected=$2
  local actual
  if command -v shasum >/dev/null 2>&1; then
    actual="$(shasum -a 256 "$file" | awk '{print $1}')"
  elif command -v sha256sum >/dev/null 2>&1; then
    actual="$(sha256sum "$file" | awk '{print $1}')"
  else
    echo "need shasum or sha256sum to verify downloads" >&2
    return 1
  fi
  [[ "$actual" == "$expected" ]]
}

download_if_needed() {
  local dest=$1 url=$2 expected_sha=$3 label=$4
  if [[ -s "$dest" ]] && python3 "$ROOT/scripts/check-runtime-inputs.py" "$dest" >/dev/null 2>&1; then
    echo "$label ready"
    return 0
  fi
  if [[ -z "$url" || -z "$expected_sha" ]]; then
    echo "no pinned download for $label (expected at $dest)" >&2
    exit 1
  fi
  mkdir -p "$(dirname "$dest")"
  local tmp="$dest.part.$$"
  trap 'rm -f "$tmp"' RETURN
  echo "downloading $label"
  curl -fL --retry 3 --connect-timeout 30 -o "$tmp" "$url"
  if ! verify_sha256 "$tmp" "$expected_sha"; then
    echo "$label checksum mismatch: expected $expected_sha" >&2
    rm -f "$tmp"
    return 1
  fi
  mv -f "$tmp" "$dest"
  trap - RETURN
  echo "$label downloaded to $dest"
}

if [[ "${GUEST_SKIP_SYSTEM:-0}" == 1 ]]; then
  # The caller boots its own system.img (SYSTEM_IMG); only the kernel is needed.
  :
elif [[ "$VARIANT" != "$LOCK_VARIANT" ]]; then
  # Other variants (e.g. a third-party GSI on its own branch) pin their GSI
  # in their own directory: images/system.url plus the system.img line of
  # images/sha256sum.txt. Without a pin they bring their own system.img.
  # The kernel is still the pinned one for KERNEL.
  if [[ -f "$AOSP/images/system.url" ]]; then
    SYSTEM_URL="$(tr -d '[:space:]' < "$AOSP/images/system.url")"
    SYSTEM_SHA256="$(awk '$2 == "system.img" {print $1}' "$AOSP/images/sha256sum.txt" 2>/dev/null || true)"
    download_if_needed "$SYSTEM" "$SYSTEM_URL" "$SYSTEM_SHA256" "system.img for $VARIANT"
  elif [[ ! -s "$SYSTEM" ]] || ! python3 "$ROOT/scripts/check-runtime-inputs.py" "$SYSTEM" >/dev/null 2>&1; then
    echo "no pinned runtime downloads configured for $VARIANT" >&2
    echo "add src/aosp/$VARIANT/images/system.url and sha256sum.txt, or place system.img there" >&2
    exit 1
  else
    echo "system.img for $VARIANT ready (unpinned)"
  fi
else
  download_if_needed "$SYSTEM" "$SYSTEM_URL" "$SYSTEM_SHA256" "system.img for $VARIANT"
fi
download_if_needed "$KERNEL_IMAGE" "$KERNEL_URL" "$KERNEL_SHA256" "GKI Image for $KID"
