# CIX Sky1 (Arm MVX / "Linlon MVE v8") hardware Codec2 — vendor integration.
#
# Brings up the vendor c2.cix.* Codec2 HAL that drives the MVX VPU V4L2-M2M
# devices directly, as the primary (hardware) video codec path on a native CIX
# Android device.  The gfxstream (c2.tebox.*) and VirGL (c2.mesa.*) components
# in this repo are only for emulated/QEMU devices without the VPU and are a
# different vendor image; nothing here runs on QEMU.
#
# Usage: from the device's device.mk,
#     $(call inherit-product, vendor/cix/codec2/cix-codec2.mk)
# AFTER the base/aosp products, so the CIX media_codecs ranks ahead of the
# software (c2.android.*) codecs.
#
# The HAL itself is a closed-source prebuilt shipped by CIX; see README.md for
# where it comes from.  This makefile only wires it into the vendor image.

LOCAL_CIX_CODEC2 := $(call my-dir)

# --- Prebuilt HAL service + libraries ---------------------------------------
# Module names are defined by vendor/cix_proprietary/android_rootfs/codec2/
# Android.bp (soong-cix_prebuild_codec2).  They install into /vendor.
PRODUCT_PACKAGES += \
    android.hardware.media.c2@1.0-cix-service \
    libcix_c2component_store \
    libcix_v4l2_codec2_common \
    libcix_v4l2_codec2_components \
    libcix_v4l2_codec2_mapper \
    libcix_v4l2_codec2_vendor_allocator \
    libc2plugin_store

# --- MVX VPU kernel module --------------------------------------------------
# The V4L2-M2M decoder (mvxdec) and encoder (mvxenc) nodes come from the MVX /
# Linlon out-of-tree module **amvx.ko** (CONFIG_VIDEO_LINLON), built by
# vendor/.../vpu_driver/vpu.mk (make target `vpu` -> driver/ `mono_v4l2`), which
# copies amvx.ko to $(TARGET_OUT_INTERMEDIATES)/VPU_OBJ.  It is an OOT module,
# not a soong/PRODUCT_PACKAGES target, so install it as a vendor kernel module
# (load order / .ko path per your kernel build):
#   BOARD_VENDOR_KERNEL_MODULES += $(TARGET_OUT_INTERMEDIATES)/VPU_OBJ/amvx.ko
# and hook the `vpu` target into the device's kernel-module build.

# --- MVX VPU firmware (.fwb) ------------------------------------------------
# 16 prebuilt_firmware modules from vendor/cix_proprietary/android_rootfs/vpu/
# Android.bp; each installs to /vendor/firmware keeping its basename.  The
# module name equals the filename.
PRODUCT_PACKAGES += \
    h264dec.fwb h264enc.fwb \
    hevcdec.fwb hevcenc.fwb \
    vp9dec.fwb  vp9enc.fwb \
    vp8dec.fwb  vp8enc.fwb \
    av1dec.fwb \
    mpeg2dec.fwb mpeg4dec.fwb \
    vc1dec.fwb avsdec.fwb avs2dec.fwb \
    jpegdec.fwb jpegenc.fwb

# --- Codec2 configuration ---------------------------------------------------
# media_codecs.xml (this overlay) includes the CIX C2 list FIRST so c2.cix.*
# wins over the software codecs for every type the hardware supports.  The
# component list (media_codecs_c2.xml) and name->so map (cix_codecs_register.xml)
# are vendored verbatim from vendor/cix_proprietary/android_rootfs/codec2/vendor/etc.
# If your BSP already installs those two to /vendor/etc, drop their lines here to
# avoid a duplicate PRODUCT_COPY_FILES destination.
PRODUCT_COPY_FILES += \
    $(LOCAL_CIX_CODEC2)/etc/media_codecs.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs.xml \
    $(LOCAL_CIX_CODEC2)/etc/media_codecs_c2.xml:$(TARGET_COPY_OUT_VENDOR)/etc/media_codecs_c2.xml \
    $(LOCAL_CIX_CODEC2)/etc/cix_codecs_register.xml:$(TARGET_COPY_OUT_VENDOR)/etc/cix_codecs_register.xml

# --- SELinux ----------------------------------------------------------------
# The HAL runs in the stock mediacodec domain; only the service-exec label and
# the /dev/video* codec-node labels are added (see sepolicy/).
BOARD_VENDOR_SEPOLICY_DIRS += $(LOCAL_CIX_CODEC2)/sepolicy

# --- Device-node permissions ------------------------------------------------
PRODUCT_COPY_FILES += \
    $(LOCAL_CIX_CODEC2)/ueventd.cix-codec2.rc:$(TARGET_COPY_OUT_VENDOR)/etc/ueventd.cix-codec2.rc

# --- Codec2 (C2) over the legacy OMX path -----------------------------------
PRODUCT_VENDOR_PROPERTIES += \
    debug.stagefright.ccodec=4
