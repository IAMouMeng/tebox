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
      flex bison e2fsprogs cpio rsync patchelf file ripgrep busybox-static \
      libvulkan-dev libvulkan1 vulkan-tools mesa-vulkan-drivers libclang-dev
    # Rust for rutabaga_gfx (.ci/build-rutabaga.sh): per user, not root, and
    # after apt so curl exists. bindgen-cli needs libclang-dev above.
    export PATH="$HOME/.cargo/bin:$PATH"
    command -v cargo >/dev/null 2>&1 ||
      curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs | sh -s -- -y --profile minimal
    command -v bindgen >/dev/null 2>&1 || cargo install bindgen-cli
    ;;
  *) echo 'Only macOS and Linux are supported' >&2; exit 1 ;;
esac
python3 -m venv "$ROOT/out/ci-venv"
"$ROOT/out/ci-venv/bin/python" -m pip install -r "$ROOT/.ci/requirements.txt"
echo "Use PATH=$ROOT/out/ci-venv/bin:\$PATH for the build."
