# CIX Sky1 / CD8180 — vendor driver integrations

SoC: **CIX CD8180 "Sky1" / P1** (Armv9.2, 12-core), reference board **Radxa
Orion O6**. These overlays target **native Android on CIX** (not QEMU).

## BSP

CIX ships an official Android 14 BSP via Radxa:

```
repo init -u https://github.com/radxa/cix-android-manifests.git \
     -b cix_radxa_o6_rc2 -m cix_radxa_o6_release.xml
repo sync
# lunch sky1_orion_o6-ap2a-userdebug   → out/target/product/sky1_orion_o6
```

- All vendor source lives in the GitLab group **`gitlab.com/cix-android14`**
  (remotes: cix, cix_open, cix_prop, cix_android, cix_firmware, …; CIX projects
  are on branch **`cix-radxa-o6`**).
- Open HALs: `vendor/cix_opensource/{vpu/vpu_driver, gpu, camera_hal, isp,
  npu, audio, …}`. Closed prebuilts + firmware: `vendor/cix_proprietary`.
- Device configs / sepolicy / seccomp: `device/cix`.

Syncing the full BSP already contains everything the overlays below reference;
the per-driver `manifest-*.xml` fragments are for pulling only the needed repos
into another tree.

## Hardware notes

- **VPU**: Arm **Linlon MVE v8** (a.k.a. Mali-V / "MVX"). Kernel OOT module
  **`amvx.ko`** (`CONFIG_VIDEO_LINLON`), V4L2-M2M, nodes `mvxdec` + `mvxenc`
  (`/dev/video9` / `/dev/video12` on CIX Android). Firmware: per-codec `.fwb`.
- Decode H.264/HEVC/AV1/VP9/VP8/MPEG-2/MPEG-4/H.263/JPEG; encode
  H.264/HEVC/VP9/VP8/JPEG.

## Drivers here

- [`codec2/`](codec2/README.md) — the `c2.cix.*` hardware Codec2 HAL
  (`android.hardware.media.c2@1.0-cix-service`) on the MVX VPU.

To add another CIX driver (camera, NPU, ISP, …), create a sibling dir
following the `codec2/` pattern.
