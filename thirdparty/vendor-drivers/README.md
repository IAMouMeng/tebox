# vendor-drivers — OEM ("原厂") Android HAL / driver integrations

Self-contained overlays that wire **vendor (OEM) hardware drivers and HALs**
into the **vendor partition** of a *native* Android build for that SoC. Each
overlay is portable: drop it into the device tree, `inherit-product` its `.mk`,
and it brings up the real hardware path (e.g. the OEM Codec2 HAL on the SoC's
VPU) as the primary — the gfxstream / VirGL emulation paths in this repo are
only for QEMU / unsupported devices and are a *different* vendor image.

## Layout convention

```
vendor-drivers/
  <vendor>/                 # one per SoC vendor (cix, …)
    README.md               # vendor-common: BSP manifest, SoC, kernel branch
    <driver>/               # one per driver / HAL (codec2, camera, npu, …)
      README.md             # what it is, where the (often closed) blobs come from
      <name>.mk             # inherit-product from device.mk; PRODUCT_* wiring
      manifest-<name>.xml   # local manifest fragment: repos to sync
      etc/                  # configs copied to /vendor/etc
      sepolicy/             # file_contexts + *.te (additive to the BSP's)
      ueventd.*.rc          # device-node ownership/perms
```

## Rules

- **No proprietary blobs here.** These overlays are the open "glue" (makefiles,
  configs, sepolicy). The closed `.so` / `.ko` / firmware stay in the vendor's
  own BSP repos; the `manifest-*.xml` fragment pulls them. Keep that boundary.
- Each `<driver>/` must build and be droppable on its own.
- Config copies (`etc/`) are verbatim copies of the vendor's files where one
  exists; note the source and branch at the top of each so they can be refreshed.
- SELinux is additive: reuse stock domains (e.g. `mediacodec`) and only add the
  labels/allows the hardware nodes need; don't redeclare types the BSP owns.

## Adding a new driver

Copy an existing `<vendor>/<driver>/` as a template (the `cix/codec2` one is the
reference), then adjust the manifest fragment, `PRODUCT_PACKAGES`, configs,
nodes and sepolicy for the new HAL. Add a new `<vendor>/` dir for a new SoC.

## Present

- [`cix/`](cix/README.md) — CIX Sky1 / CD8180 (Arm Linlon MVE v8 VPU, etc.)
  - `cix/codec2` — the `c2.cix.*` hardware Codec2 HAL on the MVX VPU.
