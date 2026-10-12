#!/usr/bin/env bash
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
case "$(uname -s)" in
  Darwin)
    brew install meson ninja pkgconf glib pixman sdl2 sdl2_image sdl2_ttf libslirp \
      libpng jpeg-turbo zstd mesa e2fsprogs libusb
    ;;
  Linux)
    SUDO=()
    [[ "$(id -u)" == 0 ]] || SUDO=(sudo)
    # ${SUDO[@]+...} also supports macOS Bash 3's empty-array nounset behavior.
    ${SUDO[@]+"${SUDO[@]}"} apt-get update
    ${SUDO[@]+"${SUDO[@]}"} apt-get install -y --no-install-recommends \
      build-essential clang git ca-certificates curl xz-utils unzip zip \
      python3 python3-venv python3-pip ninja-build pkg-config \
      libglib2.0-dev libpixman-1-dev libsdl2-dev libsdl2-image-dev libsdl2-ttf-dev \
      libslirp-dev libepoxy-dev libegl1-mesa-dev libgles2-mesa-dev \
      libgl1-mesa-dev libgbm-dev libdrm-dev libx11-dev libx11-xcb-dev \
      libxext-dev libxfixes-dev libxrandr-dev libxi-dev \
      zlib1g-dev libpng-dev libjpeg-dev libzstd-dev libusb-1.0-0-dev \
      flex bison e2fsprogs cpio rsync patchelf file ripgrep busybox-static
    ;;
  *) echo 'Only macOS and Linux are supported' >&2; exit 1 ;;
esac
python3 -m venv "$ROOT/out/ci-venv"
"$ROOT/out/ci-venv/bin/python" -m pip install -r "$ROOT/.ci/requirements.txt"
echo "Use PATH=$ROOT/out/ci-venv/bin:\$PATH for the build."
