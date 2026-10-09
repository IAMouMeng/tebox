---
name: gki-build
description: Compile this project's ARM64 QEMU host and Android Mesa/HAL/vendor/initramfs images on macOS or Linux.
---

Locate the GKI workspace and read `AGENTS.md` and `.ci/README.md`. Use the user's
selected workspace as the root if this skill was imported elsewhere.

## Choose the target

| Target | Host requirement | Entry |
| --- | --- | --- |
| QEMU + libepoxy + VirGL | macOS ARM64 or Linux ARM64 | `.ci/build-host.sh` or `.ci/build-libepoxy.sh` → `build-virglrenderer.sh` → `build-qemu.sh` |
| Android ARM64 Mesa / HALs / vendor / initramfs | Linux x86_64 for official NDK (macOS can use `darwin-x86_64` NDK tools) | `.ci/build-android.sh` or the scripts below |
| HALs only | NDK + `scripts/env.sh` | `scripts/build-hals.sh` then vendor/initramfs pack |

Never point a Linux build at `darwin-x86_64` toolchains. Do not run Linux x86_64
NDK binaries natively on ARM64 without an explicit compatibility layer.

## Guest compile order

1. `source scripts/env.sh` (sets `VARIANT`, `NDK`, `GSI_LIBS`, prefixes)
2. `scripts/extract-gsi-libs.sh` when binder/NDK stubs are missing
3. `.ci/build-libdrm-android.sh` → `.ci/build-mesa-android.sh`
4. `scripts/build-hals.sh`
5. Stop QEMU if running → `scripts/build-vendor-img.sh` → `scripts/build-initramfs.sh`
   (or just `./run`, which rebuilds stale images automatically)

Outputs land in `out/` (scratch) and tracked `prebuilts/` (host + android-arm64
installs). Variant `system.img` / `vendor.img` / `initramfs.img` / `vendor`
live under `src/aosp/<variant>/images/`; GSI link libs under
`src/aosp/<variant>/prebuilts/gsi-lib64/`; soft HALs under `hardware/`.

## Local CI helpers

```bash
bash .ci/install-deps.sh
export PATH="$PWD/out/ci-venv/bin:$PATH"
python3 .ci/validate.py
bash .ci/build-host.sh      # ARM64 host only
python3 .ci/package-systems.py  # pack vendor/init and bootable system archives
bash .ci/build-android.sh   # Linux x86_64 only; not a GitHub Actions job
```

## Reporting

State the exact commands run, artifact paths (`prebuilts/…`, `dist/…`,
`images/*.img`), and whether you only compiled or also booted. Do not claim
an untested OS/arch passed. Do not initialize or publish Git unless asked.
