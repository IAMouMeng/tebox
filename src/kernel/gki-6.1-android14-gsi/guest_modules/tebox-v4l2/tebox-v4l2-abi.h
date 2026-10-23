/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tebox-v4l2 guest<->host wire ABI. MUST stay byte-for-byte in sync with the
 * QEMU device's qemu/hw/display/tebox-v4l2-proto.h. Kernel-side copy (uses
 * __u* types). See thirdparty/vendor-drivers/cix/codec2/PASSTHROUGH-DESIGN.md.
 */
#ifndef TEBOX_V4L2_ABI_H
#define TEBOX_V4L2_ABI_H

#include <linux/types.h>

#define TEBOX_V4L2_VIRTIO_ID 60

enum { TEBOX_V4L2_VQ_CMD = 0, TEBOX_V4L2_VQ_EVT = 1, TEBOX_V4L2_VQ_MAX = 2 };

enum { TEBOX_V4L2_NODE_DECODER = 0, TEBOX_V4L2_NODE_ENCODER = 1 };

enum {
	TEBOX_V4L2_OP_QUERYCAP = 1,
	TEBOX_V4L2_OP_ENUM_FMT = 2,
	TEBOX_V4L2_OP_S_FMT = 3,
	TEBOX_V4L2_OP_G_FMT = 4,
	TEBOX_V4L2_OP_REQBUFS = 5,
	TEBOX_V4L2_OP_QBUF = 6,
	TEBOX_V4L2_OP_STREAMON = 7,
	TEBOX_V4L2_OP_STREAMOFF = 8,
	TEBOX_V4L2_OP_S_CTRL = 9,
	TEBOX_V4L2_OP_G_CTRL = 10,
	TEBOX_V4L2_OP_SUBSCRIBE_EVENT = 11,
	TEBOX_V4L2_OP_OPEN = 12,
	TEBOX_V4L2_OP_CLOSE = 13,
	TEBOX_V4L2_OP_DQBUF = 14,
	TEBOX_V4L2_OP_TRY_FMT = 15,
	TEBOX_V4L2_OP_ENCODER_CMD = 16,
	TEBOX_V4L2_OP_IOCTL_TRACE = 17,
};

struct tebox_v4l2_encoder_cmd {
	__u32 cmd;
	__u32 flags;
	__u32 is_try;
	__u32 reserved;
} __packed;

/* Data-plane transport convention: buffer payloads ride as extra scatter-gather
 * entries on the CMD vq after the header structs (see qemu proto header).
 *   OP_QBUF dir=OUTPUT: out-sg [cmd_hdr+qbuf][coded bytes = plane_bytesused[0]]
 *   OP_QBUF dir=CAPTURE: out-sg [cmd_hdr+qbuf] (no data)
 *   OP_DQBUF dir=CAPTURE: in-sg [cmd_resp+dqbuf][NV12 bytes]; -EAGAIN if none
 *   OP_DQBUF dir=OUTPUT: returns consumed input index (no data); -EAGAIN */
struct tebox_v4l2_dqbuf {
	__u32 dir;
	__u32 index;
	__u32 num_planes;
	__u32 plane_bytesused[4];
	__u64 timestamp_us;
	__u32 flags;
	__u32 reserved;
} __packed;

enum { TEBOX_V4L2_DIR_OUTPUT = 0, TEBOX_V4L2_DIR_CAPTURE = 1 };

struct tebox_v4l2_cmd_hdr {
	__u32 op;
	__u32 stream;
	__u32 seq;
	__u32 len;
} __packed;

struct tebox_v4l2_cmd_resp {
	__u32 seq;
	__s32 result;
	__u32 len;
	__u32 reserved;
} __packed;

struct tebox_v4l2_open {
	__u32 node;
} __packed;

struct tebox_v4l2_format {
	__u32 dir;
	__u32 pixelformat;
	__u32 width;
	__u32 height;
	__u32 num_planes;
	__u32 plane_size[4];
	__u32 plane_stride[4];
} __packed;

struct tebox_v4l2_enum_fmt {
	__u32 dir;
	__u32 index;
	__u32 pixelformat;
	__u32 flags;
} __packed;

struct tebox_v4l2_reqbufs {
	__u32 dir;
	__u32 count;
} __packed;

struct tebox_v4l2_qbuf {
	__u32 dir;
	__u32 index;
	__u32 resource_id;
	__u32 num_planes;
	__u32 plane_offset[4];
	__u32 plane_bytesused[4];
	__u64 timestamp_us;
	__u32 flags;
	__u32 reserved;
} __packed;

struct tebox_v4l2_dir_arg {
	__u32 dir;
} __packed;

struct tebox_v4l2_ctrl {
	__u32 id;
	__s32 value;
} __packed;

enum {
	TEBOX_V4L2_EVT_DQBUF = 1,
	TEBOX_V4L2_EVT_SOURCE_CHANGE = 2,
	TEBOX_V4L2_EVT_EOS = 3,
	TEBOX_V4L2_EVT_ERROR = 4,
};

struct tebox_v4l2_event {
	__u32 type;
	__u32 stream;
	__u32 dir;
	__u32 index;
	__u32 num_planes;
	__u32 plane_bytesused[4];
	__u64 timestamp_us;
	__u32 flags;
	__u32 reserved;
} __packed;

#endif /* TEBOX_V4L2_ABI_H */
