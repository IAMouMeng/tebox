#!/usr/bin/env bash
# Lunch menu for a packed tebox directory.
# One system boots immediately. Several systems print a menu; Enter picks 1.
#
# Usage:
#   ./run
#   ./run <variant>
#   ./run <n>
#   VARIANT=... ./run
set -euo pipefail
ROOT="$(cd "$(dirname "$0")" && pwd)"
cd "$ROOT"

VARIANTS=()
while IFS= read -r name; do
  [[ -n "$name" ]] && VARIANTS+=("$name")
done <<EOF
$(for d in "$ROOT"/src/aosp/*/; do
    [[ -d "$d" ]] || continue
    name="$(basename "$d")"
    [[ -f "$d/KERNEL" ]] || continue
    [[ -d "$d/images" || -d "$d/qemu" ]] || continue
    printf '%s\n' "$name"
  done | LC_ALL=C sort)
EOF

if ((${#VARIANTS[@]} == 0)); then
  echo "no systems under src/aosp/*/ (need KERNEL + images)" >&2
  exit 1
fi

pick=
if [[ -n "${VARIANT:-}" ]]; then
  pick="$VARIANT"
elif (($# >= 1)); then
  pick="$1"
fi

if [[ -z "$pick" ]] && ((${#VARIANTS[@]} == 1)); then
  pick="${VARIANTS[0]}"
fi

if [[ -z "$pick" ]] || [[ "$pick" =~ ^[0-9]+$ ]]; then
  host="$(uname -s)/$(uname -m)"
  echo
  echo "You're building on $host"
  echo
  echo "Lunch menu... pick a combo:"
  echo
  i=1
  default=1
  for v in "${VARIANTS[@]}"; do
    kid="$(tr -d '[:space:]' < "$ROOT/src/aosp/$v/KERNEL" 2>/dev/null || echo '?')"
    mark=
    (( i == default )) && mark=" (default)"
    printf '     %d. %s%s\n         kernel: %s\n' "$i" "$v" "$mark" "$kid"
    i=$((i + 1))
  done
  echo
  if [[ -n "${pick:-}" && "$pick" =~ ^[0-9]+$ ]]; then
    idx="$pick"
  elif [[ ! -t 0 ]]; then
    idx=$default
    echo "Which would you like? [$default] $default (default)"
  else
    printf "Which would you like? [%d] " "$default"
    read -r idx || true
    [[ -z "${idx:-}" ]] && idx=$default
  fi
  if ! [[ "$idx" =~ ^[0-9]+$ ]] || (( idx < 1 || idx > ${#VARIANTS[@]} )); then
    echo "invalid selection: $idx" >&2
    exit 1
  fi
  pick="${VARIANTS[$((idx - 1))]}"
fi

pick="${pick%/}"
pick="${pick##*/}"

ok=0
for v in "${VARIANTS[@]}"; do
  [[ "$v" == "$pick" ]] && ok=1 && break
done
if (( ok == 0 )); then
  echo "unknown variant: $pick" >&2
  echo "available:" >&2
  printf '  %s\n' "${VARIANTS[@]}" >&2
  exit 1
fi

export VARIANT="$pick"
export QEMU="${QEMU:-$ROOT/qemu}"
echo "==> lunch $VARIANT"
exec bash "$ROOT/scripts/boot-qemu.sh" "$VARIANT"
