# scripts/

Runtime and guest-image scripts used by `./run` and local HAL work.

| Script | Purpose |
| --- | --- |
| `env.sh` | Shared paths (NDK, prebuilts, `HOST_ID`). Sourced by other scripts. |
| `ensure-guest-downloads.sh` | Download `system.img` + GKI `Image` from mirror when missing. |
| `ensure-runtime-imgs.sh` | Rebuild `vendor.img` / `initramfs.img` when inputs are newer. |
| `boot-qemu.sh` | Launch QEMU for one variant. |
| `build-vendor-img.sh` | Pack `qemu/vendor` (+ Mesa) into `images/vendor.img`. |
| `build-initramfs.sh` | Build initramfs (busybox + kernel modules + overlays). |
| `build-hals.sh` | Rebuild and install the standard soft HAL set. |
| `extract-gsi-libs.sh` | Extract binder/NDK link libs from the variant GSI. |
| `check-runtime-inputs.py` | Reject missing or stale pointer files before boot/pack. |
| `qemu-console.py` | Run commands on the opt-in QEMU debug console. |
| `adb-scrcpy.py` | ADB + scrcpy smoke test against a running guest. |
| `r1p-deploy.sh` | Build the CIX Sky1 (MYT R1P) runtime image locally (`.ci/cix-r1p/Dockerfile`) and run it on a remote R1P: `build`, `push`, `images`, `start`, `stop`, `logs`, `all`. `bin` rebuilds only tebox-api / tebox-cast (in the `tebox-r1p-builder` image) and uploads them without a new image. Only binaries and images are uploaded. |
| `fetch-host-drivers.sh <platform>` | Restore missing packages in `thirdparty/host-drivers/<platform>` from its `SOURCES` list and verify every checksum (run by `r1p-deploy.sh build`). |
| `cix-r1p-start.sh` | Container entrypoint on the R1P: Mali GLES (`egl-headless,gl=es`), adb on `0.0.0.0:$ADB_PORT`, `androidboot.tebox.video_encode=1` (VirGL V4L2 `mvxenc` H.264). |
| `cix-mali-env.sh` | CIX Mali userspace environment sourced by the R1P entrypoint. |

`boot-qemu.sh` extras: `ADB_BIND` (default `127.0.0.1`) and `QEMU_EXTRA_APPEND`
(extra kernel/androidboot tokens). `androidboot.tebox.video_encode=1` lists
`c2.mesa.avc.encoder` via `/vendor/etc/media_codecs_tebox_video.xml`; use it only
when the host VirGL has a video encoder (macOS VideoToolbox, Linux V4L2/VA-API).

Host/Android compile entry points live under [`.ci/`](../.ci/README.md)
(`build-host.sh`, `build-android.sh`, `build-qemu.sh`, …).
