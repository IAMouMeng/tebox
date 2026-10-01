# Reproducible CI builds

## Goals

- Native **QEMU + libepoxy + VirGL** packages for **macOS ARM64** and **Linux ARM64**
- Portable **skill ZIPs** for WorkBuddy / local importers
- Guest images, HALs and the GKI kernel stay as tracked prebuilts under `src/`
- No repository write permissions beyond `actions/upload-artifact`

## Workflow

`.github/workflows/build.yml`

1. **validate** — `python .ci/validate.py` + `scripts/package-skills.py`
2. **host** matrix — `macos-26` and `ubuntu-24.04-arm`  
   build QEMU, then `.ci/package-systems.py` packs each `src/aosp/<variant>`

`ubuntu-24.04` only runs validation. The host job does not recompile guest HALs,
Mesa, or BusyBox. It does rebuild `vendor.img` and `initramfs.img`, then packs
them with the tracked `system.img` and kernel. The host checkout uses Git LFS.

## Local parity

```bash
bash .ci/install-deps.sh
export PATH="$PWD/out/ci-venv/bin:$PATH"
python3 .ci/validate.py
bash .ci/build-host.sh          # requires HOST_ARCH=arm64
python3 .ci/package-systems.py  # vendor/init + bootable system archives
```

`scripts/env.sh` defines `HOST_ID`, NDK/SDK paths and prebuilt prefixes.
Host/guest trees (`qemu/`, `thirdparty/`, …) are tracked in-repo; CI builds them
in place.

## Pins

| Lock | Contents |
| --- | --- |
| `.ci/guest.lock.json` | GSI zip URL/sha256, busybox, NDK versions, kernel/module digests |

`system.img` and GKI `Image`/`.ko` are committed via Git LFS (`.gitattributes`).
`vendor.img` and `initramfs.img` stay gitignored and are rebuilt by
`scripts/ensure-runtime-imgs.sh` while packing. Install `git-lfs` before cloning
or committing the tracked binaries.

## Artifacts

| Artifact | Notes |
| --- | --- |
| `qemu-gki-<os>-arm64.tar.gz` | Relocatable `./qemu` wrapper; smoke-tests `--version` and `virtio-gpu-gl-pci` |
| `tebox-<os>-arm64` | One `tebox-<variant>-<os>-arm64.tar.gz` per system. `./run` starts the only system, or shows the lunch menu. Extract another variant for the same host into the same directory to add it |
| `tebox-vendor-init.tar.gz` | Every variant’s `vendor.img` and `initramfs.img` under `src/aosp/<variant>/images/`. No `system.img`. Uploaded from the Linux ARM64 job |
| `project-ai-skills` | `dist/skills/*.zip` |

Failure logs upload `out/*.log` and meson logs only.

## Non-goals

- Real GPU/display verification on GitHub runners
- Uploading the whole workspace or `out/` userdata images from CI artifacts
- Initializing or publishing a Git remote from CI
