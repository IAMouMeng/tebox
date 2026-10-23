# Camera provider (CVD AIDL port)

NDK AIDL port of Cuttlefish `guest/hals/camera` (HIDL 2.7 vsock) for this
QEMU GSI. `getCameraIdList` is empty; `setCallback` announces
`device@3.4/external/0` via `cameraDeviceStatusChange`, the same way CVD
does when a vsock client connects.

This guest has no vsock camera host. The HAL simulates that host with a
moving test pattern. If the existing `tebox.camera` virtio-serial port
already has a viewer, those NV12 frames are used instead. Do not add a
vsock device.

Build/install: `aidl-provider/build.sh` + `aidl-provider/install.sh`
(`PRODUCT_PACKAGES += android.hardware.camera.provider-service`).
