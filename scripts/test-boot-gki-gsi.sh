#!/usr/bin/env bash
# Deprecated wrapper — use ./run from the repo root.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
if (($# >= 1)); then
  exec "$ROOT/run" "$1"
fi
exec "$ROOT/run"
