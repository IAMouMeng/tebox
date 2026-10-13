#!/usr/bin/env bash
# Restore and verify thirdparty/host-drivers/<platform> from its SOURCES file.
# Usage: scripts/fetch-host-drivers.sh <platform>
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIR="$ROOT/thirdparty/host-drivers/${1:?usage: $0 <platform>}"
[[ -f "$DIR/SOURCES" ]] || { echo "no $DIR/SOURCES" >&2; exit 1; }
while read -r file sum url; do
  [[ -z "$file" || "$file" == \#* ]] && continue
  if [[ ! -f "$DIR/$file" ]]; then
    echo "download: $file"
    curl -fL -o "$DIR/$file.part" "$url"
    mv "$DIR/$file.part" "$DIR/$file"
  fi
  [[ "$(shasum -a 256 "$DIR/$file" | cut -d' ' -f1)" == "$sum" ]] || {
    echo "checksum mismatch: $DIR/$file" >&2; exit 1; }
done < "$DIR/SOURCES"
