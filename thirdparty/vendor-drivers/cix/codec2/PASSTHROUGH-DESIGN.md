# OriginOS → CIX c2.cix over QEMU: MVX V4L2 passthrough design

Goal: OriginOS (QEMU guest on R1P) runs the **stock CIX `c2.cix` Codec2 HAL**,
which drives the MVX VPU through a real V4L2 `/dev/video` node in the guest —
replacing the gfxstream (`c2.tebox`) / VirGL (`c2.mesa`) forwarding shims.

## Why a custom transport is required (facts from R1P, 2026-10-19)

- MVX platform device `14230000.vpu` (driver `amvx_dev`) has **no IOMMU group**;
  R1P has no `iommu`/`vfio` on the kernel cmdline and no `vfio-platform` driver.
  → **VFIO / device passthrough is impossible** (VFIO needs an IOMMU group).
- Guest GKI kernel `6.1.172-android14` has full V4L2 M2M / videobuf2 but **no
  `CONFIG_VIRTIO_VIDEO`** and no `/dev/video*`.
- Host codec today: guest `c2.tebox` speaks a private protocol over the
  `tebox.vdec`/`tebox.venc` virtio-serial ports to a host backend
  (`tebox-cast` on R1P → mvx; VideoToolbox on macOS). `c2.cix` cannot use that —
  it speaks **V4L2** to `/dev/video`.

So the guest must be given a real V4L2 M2M node backed by the host mvx. With no
IOMMU and no stock virtio-video driver, the only route is a **bespoke virtio
V4L2 passthrough**: QEMU device ⇄ guest kernel module ⇄ host `/dev/video0,1`.

## Architecture (decided)

```
 guest (OriginOS)                         host (R1P container)
 ┌───────────────────────┐                ┌──────────────────────────┐
 │ c2.cix HAL (stock)     │ open/ioctl     │ QEMU tebox-v4l2 device    │
 │   ↓ V4L2               │ /dev/video9,12 │   backend opens host       │
 │ tebox_v4l2.ko (new)    │ ◄── virtqueue ─►│   /dev/video0 (mvxdec)     │
 │   V4L2 M2M dev, marshal│  cmd + buffers │   /dev/video1 (mvxenc),    │
 │   ioctls/buffers over  │                │   replays ioctls 1:1,      │
 │   virtio               │                │   maps MMAP buffers        │
 └───────────────────────┘                └──────────────────────────┘
```

- **Transport**: one virtio device (`tebox-v4l2`, TYPE_VIRTIO_DEVICE, like the
  existing `hw/display/tebox-*`), two virtqueues — a command queue (serialized
  VIDIOC_* ioctls + their args) and a buffer/dataplane queue (coded-in /
  NV12-out payloads; guest pages shared, host does the mvx MMAP copy). Keep a
  slim, 1:1 V4L2 ioctl subset (QUERYCAP, ENUM_FMT, S/G_FMT, REQBUFS, QUERYBUF,
  QBUF, DQBUF, STREAMON/OFF, S/G_CTRL, SUBSCRIBE_EVENT/DQEVENT) — not the full
  virtio-video OASIS spec, since we own both ends.
- **Guest module** `tebox_v4l2.ko`: registers two `video_device` M2M nodes
  (decoder, encoder) on the V4L2 M2M framework already in GKI; forwards each
  ioctl over the command vq; backs buffers with videobuf2-dma-contig and ships
  payloads over the dataplane vq. Shipped as a vendor `.ko`, loaded from the
  variant initramfs (this repo already tracks guest `*.ko`).
- **Host backend**: opens `/dev/video0` (mvxdec) and `/dev/video1` (mvxenc),
  replays the guest's ioctls, and feeds buffers to mvx **by DMA on the guest's
  own pages** (see zero-copy below). Stateful-decoder specifics (SOURCE_CHANGE)
  pass straight through as V4L2 events, so the guest driver and c2.cix see real
  stateful behaviour.

## Performance requirement: zero-copy, no GPU, thin kernel (hard)

The codec path must add almost no CPU/GPU cost and keep the guest kernel on the
control path only — no per-frame work, no polling, no pixel copies, no GL.

- **Zero-copy buffers via virtio-gpu resource ids.** c2.cix's vendor allocator
  (`libcix_v4l2_codec2_vendor_allocator`) hands out gralloc/dmabuf buffers; on
  this stack gralloc is minigbm over virtio-gpu, so each buffer is a virtio-gpu
  blob resource with a host shadow. On QBUF the guest `tebox_v4l2.ko` sends only
  the buffer's **virtio-gpu resource id** (+ offset/len/flags) over the vq — never
  the pixels. QEMU resolves the id to the host mapping (same path as
  `mesa_pipe_video.cpp`'s `resourceIdFromFd` → virtio-gpu RESOURCE_INFO) and
  queues it to mvx with `V4L2_MEMORY_DMABUF` (or USERPTR), so **mvx DMAs the
  guest pages directly**. No memcpy on either side, no GL/GPU, no readback.
- **Thin guest kernel.** `tebox_v4l2.ko` only marshals control ioctls and buffer
  ids; it does no pixel processing and no busy-wait. DQBUF completions come back
  as a vq used-buffer notification (interrupt-driven), not polling. CPU touches
  the path only to forward a handful of small control messages per frame.
- **No GPU.** Unlike the gfx/virgl shims, nothing here reads/writes GL textures;
  the VPU does the work and talks to memory directly.
- **Guest vendor (OriginOS)**: install the `c2.cix` HAL (the `vendor-drivers/
  cix/codec2` overlay), disable the `c2.tebox`/`c2.mesa` codec2 service, point
  media_codecs at `c2.cix`. Node numbers: the guest `tebox_v4l2.ko` creates the
  decode node first → `/dev/video9`, encode → `/dev/video12` (match the overlay
  sepolicy/ueventd), or relabel to whatever the module assigns.

## Stages (each verified on R1P before the next)

1. **QEMU device + host probe**: `hw/display/tebox-v4l2.c` registers the virtio
   device; backend opens `/dev/video0,1`, logs `VIDIOC_QUERYCAP` ("Linlon Video
   device"). Wire into `scripts/boot-qemu.sh` behind a flag (R1P only). Verify:
   QEMU log shows both mvx nodes opened from inside QEMU.
2. **Guest module brings up /dev/video**: `tebox_v4l2.ko` registers the two M2M
   nodes; QUERYCAP/ENUM_FMT round-trip over the command vq. Verify: guest
   `ls /dev/video*` + a `v4l2` ioctl probe returns the mvx formats.
3. **Dataplane**: QBUF/DQBUF + STREAMON, NV12 out / coded in, SOURCE_CHANGE
   events. Verify: a raw V4L2 decode/encode of one clip in the guest.
4. **c2.cix**: drop in the vendor overlay, disable gfx c2, media_codecs → cix.
   Verify: `dumpsys media.codec` shows `c2.cix.*` decoding/encoding a real clip
   on the guest.

## Scope / honesty

This is a large, multi-component build (QEMU C device + guest kernel module +
vendor HAL) with a per-stage R1P rebuild/deploy loop; it is not a single change.
Stages 1–2 are the risky long pole. The `c2.cix` vendor overlay (stage 4) is
already written under `vendor-drivers/cix/codec2`.

Alternative that avoids all of the above: run OriginOS **natively on CIX** (no
QEMU) — then `c2.cix` opens the real `/dev/video` directly and only the vendor
overlay is needed. Chosen path here is QEMU + passthrough per request.
