# src/common/hardware — AOSP `hardware/` project

Shared vendor HAL defaults. Other common AOSP projects (`frameworks/`,
`packages/`) sit next to this directory under `src/common/` when they are
added. Device trees stay under `src/aosp/<variant>/`.

```
src/common/hardware/interfaces/<hal>/aidl/default/   # cc_binary / gki_custom + rc + vintf xml
src/common/hardware/interfaces/common.mk             # generic PRODUCT_PACKAGES
src/common/hardware/shim/                            # NDK compatibility headers
build/soong/soong.py                                 # Soong-lite (`source build/envsetup.sh && lunch && m`)
src/aosp/<variant>/device.mk                         # inherit common.mk, extra packages
src/aosp/<variant>/hardware/                         # OEM-only HALs (optional)
```

## Product

```makefile
# src/aosp/aosp_arm64-BP4A.251205.006/device.mk
$(call inherit-product, src/common/hardware/interfaces/common.mk)
```

A new AOSP variant needs that `device.mk`. `build-vendor-img.sh` runs Soong-lite
incrementally, then packs `qemu/vendor`.

## Android.bp

```
cc_binary {
    name: "android.hardware.lights-service",
    relative_install_path: "hw",
    vendor: true,
    srcs: ["lights.cpp"],
    shared_libs: [
        "libbinder_ndk",
        "liblog",
        "android.hardware.light-V2-ndk",
    ],
    init_rc: ["android.hardware.lights-service.rc"],
    vintf_fragments: ["android.hardware.lights-service.xml"],
}
```

`android.hardware.*-V<n>-ndk` in `shared_libs` generates frozen NDK AIDL (no
full Soong). QEMU-only HALs that still use a script are `gki_custom`.

## Build

```bash
source build/envsetup.sh
lunch aosp_arm64-BP4A.251205.006
m
m android.hardware.lights-service
```

Kernel drivers stay in `src/kernel/`.
