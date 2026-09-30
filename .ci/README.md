# Reproducible CI builds

## Goals

- Native **QEMU + libepoxy + VirGL** packages for **macOS ARM64** and **Linux ARM64**
- **Android ARM64** guest pieces (Mesa, HALs, `vendor.img`, initramfs, GKI Image)
- Portable **skill ZIPs** for WorkBuddy / local importers
- No repository write permissions beyond `actions/upload-artifact`

## Workflow

`.github/workflows/build.yml`

1. **validate** — `python .ci/validate.py` + `scripts/package-skills.py`
2. **host** matrix — `macos-14` → `qemu-gki-darwin-arm64.tar.gz`;  
   `ubuntu-24.04-arm` → `qemu-gki-linux-arm64.tar.gz`
3. **android** — `ubuntu-24.04` (x86_64) installs pinned NDK/build-tools, fetches
   guest inputs from `.ci/guest.lock.json`, builds ARM64 guest runtime →
   `gki-android-arm64.tar.gz` (does **not** upload `system.img`)

## Local parity

```bash
bash .ci/install-deps.sh
export PATH="$PWD/out/ci-venv/bin:$PATH"
python3 .ci/validate.py
bash .ci/build-host.sh          # requires HOST_ARCH=arm64
# On Linux x86_64 only:
bash .ci/build-android.sh
```

`scripts/env.sh` defines `HOST_ID`, NDK/SDK paths and prebuilt prefixes.
Host/guest trees (`qemu/`, `thirdparty/`, …) are tracked in-repo; CI builds them
in place.

## Pins

| Lock | Contents |
| --- | --- |
| `.ci/guest.lock.json` | GSI zip URL/sha256, busybox, NDK versions, kernel/module digests |

Boot images for the active variant (`system.img`, `vendor.img`, initramfs) and GKI
`Image`/`.ko` are committed via Git LFS (`.gitattributes`). Install `git-lfs`
before cloning or committing those binaries.

## Artifacts

| Artifact | Notes |
| --- | --- |
| `qemu-gki-<os>-arm64.tar.gz` | Relocatable `./qemu` wrapper; smoke-tests `--version` and `virtio-gpu-gl-pci` |
| `gki-android-arm64.tar.gz` | Image, koz, vendor/initramfs, boot scripts; README tells how to fetch `system.img` |
| `project-ai-skills` | `dist/skills/*.zip` |

Failure logs upload `out/*.log` and meson logs only.

## Non-goals

- Real GPU/display verification on GitHub runners
- Uploading the whole workspace or `out/` userdata images from CI artifacts
- Initializing or publishing a Git remote from CI
