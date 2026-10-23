#!/usr/bin/env bash
# QEMU / runtime helper. Guest compile: source build/envsetup.sh && lunch && m.
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
# shellcheck source=/dev/null
source "$ROOT/build/make/core/config.sh"
