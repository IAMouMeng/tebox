---
name: gki-build
description: Compile this project's ARM64 QEMU host and Android Mesa/HAL/vendor/initramfs images on macOS or Linux.
---

Locate the GKI workspace; read root `README.md`, then `.ai/RAG.md` (how to
develop), `.ai/AGENTS.md` and `.ci/README.md`. Use the user's selected workspace
as the root if this skill was imported elsewhere.

## Choose the target

| Target | Host requirement | Entry |
| --- | --- | --- |
| QEMU + libepoxy + VirGL | macOS ARM64 or Linux ARM64 | `.ci/build-host.sh` or `scripts/build-libepoxy.sh` → `build-virglrenderer.sh` → `build-qemu.sh` |
| Android ARM64 Mesa / HALs / vendor / initramfs | Linux x86_64 for official NDK (macOS can use `darwin-x86_64` NDK tools) | `.ci/build-android.sh` or the scripts below |
| HALs only | NDK | `source build/envsetup.sh && lunch <product> && m` |

Never point a Linux build at `darwin-x86_64` toolchains. Do not run Linux x86_64
NDK binaries natively on ARM64 without an explicit compatibility layer.

Variant trees live on `GSI/...` branches (`GSI/AOSP/aosp_arm64-BP4A.251205.006`
for the AOSP example). `main` has no `src/aosp/`. Checkout the GSI branch
before `lunch`, then `git checkout -b dirty/<topic>` and commit locally after
significant changes. Do not treat `scripts/build-hals.sh` stub loops as the
default; `device.mk` + `m` is the HAL path.

## Guest compile order

1. Checkout the GSI branch that owns `src/aosp/<product>/`.
2. `source build/envsetup.sh && lunch <product>` (sets `TARGET_PRODUCT`, `NDK`,
   `GSI_LIBS`; product is the `src/aosp/` folder name). `device.mk` must
   inherit `src/common/hardware/interfaces/common.mk`.
3. `m` or `m <module>` (soong-lite reads `Android.bp` from common and from
   `src/aosp/<product>/hardware/`).
4. `scripts/extract-gsi-libs.sh` when binder/NDK stubs are missing (`m` also
   extracts when needed).
5. `scripts/build-libdrm-android.sh` → `scripts/build-mesa-android.sh` when
   Mesa changed.
6. Stop QEMU if running → `scripts/build-vendor-img.sh` →
   `scripts/build-initramfs.sh` (or `./run`, which rebuilds stale images).

Outputs land in `out/` (scratch) and tracked `prebuilts/` (host + android-arm64
installs). Variant `system.img` / `vendor.img` / `initramfs.img` / `vendor`
live under `src/aosp/<variant>/images/`; GSI link libs under
`src/aosp/<variant>/prebuilts/gsi-lib64/`; shared HALs under `src/common/hardware/`.

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
