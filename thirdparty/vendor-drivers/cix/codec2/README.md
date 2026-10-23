# CIX Sky1 hardware Codec2 — native Android vendor integration

This overlay wires CIX's **own** hardware Codec2 HAL (`c2.cix.*`, which drives
the Arm MVX / "Linlon MVE v8" VPU over V4L2-M2M) into the **vendor** image of a
native CIX Android device, as the *primary* video codec path.

It is **not** for QEMU: the `c2.mesa.*` (VirGL) and `c2.tebox.*` (gfxstream)
components elsewhere in this repo are the fallback for emulated devices that
have no VPU, and live in a different vendor image. On a real CIX device only
`c2.cix.*` is present, so "only unsupported devices use gfx/virgl" is simply
*two different vendor images*, not a runtime switch.

Target: **Android 14**, product **sky1_orion_o6** (Radxa Orion O6), CIX CD8180.

## Where the HAL itself comes from (closed-source prebuilt)

The HAL is shipped by CIX as prebuilt binaries — do **not** expect source:

- Repo `gitlab.com/cix-android14/cix_proprietary`, path
  `android_rootfs/codec2/`. Provides, via `soong-cix_prebuild_codec2`:
  - service `android.hardware.media.c2@1.0-cix-service` (HIDL c2@1.0, mediacodec domain)
  - `libcix_v4l2_codec2_components.so` (the `c2.cix.*` components), plus
    `libcix_c2component_store.so`, `libcix_v4l2_codec2_{common,mapper,vendor_allocator}.so`,
    `libc2plugin_store.so`
  - `vendor/etc/media_codecs_c2.xml`, `vendor/etc/cix_codecs_register.xml`
  - init `vendor/etc/init/android.hardware.media.c2@1.0-cix-service.rc`
  - VPU `.fwb` firmware under `android_rootfs/vpu/vendor/firmware/`
- Kernel driver `gitlab.com/cix-android14/vpu_driver` — the OOT module
  **`amvx.ko`** (`CONFIG_VIDEO_LINLON`, Arm Linlon/MVE V4L2-M2M → mvxdec/mvxenc),
  built by its `vpu.mk` (`vpu` target → `driver/` `mono_v4l2`), copied to
  `$(TARGET_OUT_INTERMEDIATES)/VPU_OBJ/amvx.ko`.
- Firmware: 16 `.fwb` blobs (`h264dec/enc`, `hevcdec/enc`, `vp9dec/enc`,
  `vp8dec/enc`, `av1dec`, `mpeg2dec`, `mpeg4dec`, `vc1dec`, `avsdec`, `avs2dec`,
  `jpegdec/enc`) installed to `/vendor/firmware` by `prebuilt_firmware` modules
  in `cix_proprietary/android_rootfs/vpu/Android.bp` (module name = filename).
- `gitlab.com/cix-android14/device/cix` carries `media_codecs_performance_c2.xml`,
  `media_profiles_default.xml`, `seccomp_policy/codec2.vendor.{base,ext}.policy`,
  sepolicy and `gralloc/vpu.xml`.

`manifest-cix-codec2.xml` pulls exactly those three projects; syncing the full
`radxa/cix-android-manifests` BSP already includes them.

## Files in this overlay (what each integration step maps to)

| step | file |
|---|---|
| pull HAL + driver + device/cix | `manifest-cix-codec2.xml` → `.repo/local_manifests/` |
| install HAL, firmware, props | `cix-codec2.mk` → `$(call inherit-product, ...)` from `device.mk` |
| rank `c2.cix.*` ahead of software | `etc/media_codecs.xml` (CIX list included first) |
| component list + limits (verbatim) | `etc/media_codecs_c2.xml` |
| name → .so map (verbatim) | `etc/cix_codecs_register.xml` |
| SELinux service + node labels | `sepolicy/file_contexts`, `sepolicy/mediacodec.te` |
| node ownership for the HAL | `ueventd.cix-codec2.rc` |

The two XMLs are exact copies of CIX's (branch cix-radxa-o6); drop their
`PRODUCT_COPY_FILES` lines from `cix-codec2.mk` if your BSP already installs
them, to avoid a duplicate destination.

## Device nodes (decided here)

The closed `.so` opens the VPU nodes itself; on CIX Android they enumerate as
**`/dev/video9` (decode, mvxdec)** and **`/dev/video12` (encode, mvxenc)**.
This overlay labels and owns those two for the `mediacodec` domain. If the VPU
enumerates elsewhere on your board, change the numbers in
`sepolicy/file_contexts` and `ueventd.cix-codec2.rc` (grep the HAL's logcat for
the `VIDIOC_QUERYCAP` card "Linlon Video device" to confirm).

## Integrate

1. `cp manifest-cix-codec2.xml <tree>/.repo/local_manifests/` and `repo sync`
   (or sync the full `cix-android-manifests` BSP).
2. Place this overlay at `vendor/cix/codec2/` in the tree.
3. In the device's `device.mk`, after the base/aosp products:
   `$(call inherit-product, vendor/cix/codec2/cix-codec2.mk)`
4. Build `amvx.ko` into the vendor kernel modules
   (`BOARD_VENDOR_KERNEL_MODULES += .../VPU_OBJ/amvx.ko`, hook the `vpu` target
   into your kernel-module build). The firmware `.fwb` is pulled by the
   `PRODUCT_PACKAGES` list in `cix-codec2.mk` (installs to `/vendor/firmware`).
5. Build; the HAL lands at `/vendor/bin/hw/android.hardware.media.c2@1.0-cix-service`.

## Verify it uses c2.cix (not software / gfx / virgl)

- `adb shell dumpsys media.player | grep c2.cix` — the components are registered.
- Play/record, then `adb shell dumpsys media.codec` — the active codec is
  `c2.cix.<fmt>.decoder/encoder`, not `c2.android.*`.
- `adb shell ls -lZ /dev/video9 /dev/video12` — labeled `video_device`.
- `adb shell dmesg | grep -i mvx` — the MVX driver bound the nodes.
