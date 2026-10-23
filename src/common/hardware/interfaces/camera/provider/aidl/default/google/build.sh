#!/usr/bin/env bash
# Wrapper kept for the old gki_custom path. The product HAL is the CVD AIDL
# port in aidl-provider/ (no vsock device; HAL simulates the host).
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
_d="$HERE"
while [[ "$_d" != "/" && ! -f "$_d/build/hal-env.sh" ]]; do _d=$(dirname "$_d"); done
# shellcheck source=/dev/null
source "$_d/build/hal-env.sh"
bash "$HERE/../aidl-provider/build.sh"
