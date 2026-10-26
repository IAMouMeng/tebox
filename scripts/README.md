# scripts/

Runtime and guest-image scripts used by `./run` and local HAL work.

| Script | Purpose |
| --- | --- |
| `env.sh` | Shared paths (NDK, prebuilts, `HOST_ID`). Sourced by other scripts. |
| `ensure-guest-downloads.sh` | Download branch `rom.zip` (extract `system.img`) + GKI `Image` from mirror when missing. |
| `ensure-runtime-imgs.sh` | Rebuild `vendor.img` / `initramfs.img` when inputs are newer. |
| `boot-qemu.sh` | Launch QEMU for one variant. |
| `build-vendor-img.sh` | Incremental Soong-lite (when the variant has `device.mk`), then pack `qemu/vendor` (+ Mesa) into `images/vendor.img`. |
| `build-initramfs.sh` | Build initramfs (busybox + kernel modules + `qemu/init` rc + `qemu/overlay`). |
| `build-hals.sh` | Rebuild HALs (`build/soong/soong.py`). Prefer `source build/envsetup.sh && lunch && m`. |
| `extract-gsi-libs.sh` | Extract binder/NDK link libs from the variant GSI. |
| `check-runtime-inputs.py` | Reject missing or stale pointer files before boot/pack. |
| `qemu-console.py` | Run commands on the opt-in QEMU debug console. |
| `fetch-host-drivers.sh <platform>` | Restore missing packages in `thirdparty/host-drivers/<platform>` from its `SOURCES` list and verify every checksum. |
| `cix-r1p-start.sh` | Container entrypoint on the R1P: Mali GLES (`egl-headless,gl=es`), adb on `0.0.0.0:$ADB_PORT`, `androidboot.tebox.video_encode=1` (VirGL V4L2 `mvxenc` H.264). Used by the root `Dockerfile`. |
| `cix-mali-env.sh` | CIX Mali userspace environment sourced by the R1P entrypoint. |
| `r1p-deploy.sh` | Assemble the products-only context under `out/r1p-docker/` and build the root `Dockerfile` (`tebox:<ver>`). Also `push` / `start` / `export` to an R1P. CI wraps this via `.ci/build-r1p-image.sh`. |

R1P products image: root `Dockerfile` + `scripts/r1p-deploy.sh` (CI: `.ci/build-r1p-image.sh` → `dist/tebox-r1p-linux-arm64.tar.gz`).

`boot-qemu.sh` extras: `ADB_BIND` (default `127.0.0.1`) and `QEMU_EXTRA_APPEND`
(extra kernel/androidboot tokens). `androidboot.tebox.video_encode=1` lists
`c2.mesa.avc.encoder` via `/vendor/etc/media_codecs_tebox_video.xml`; use it only
when the host VirGL has a video encoder (macOS VideoToolbox, Linux V4L2/VA-API).

Host/Android compile entry points live under [`.ci/`](../.ci/README.md)
(`build-host.sh`, `build-android.sh`, `build-qemu.sh`, …).
