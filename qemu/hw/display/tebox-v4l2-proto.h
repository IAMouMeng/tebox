/*
 * tebox-v4l2 (local, not for upstream): wire protocol shared by the QEMU
 * device (hw/display/tebox-v4l2.c) and the guest kernel driver
 * (tebox_v4l2.ko). A thin virtio transport that lets a guest V4L2 M2M driver
 * drive the host MVX VPU (/dev/video0 mvxdec, /dev/video1 mvxenc) so the stock
 * CIX c2.cix Codec2 HAL works unmodified.
 *
 * Design goals (see PASSTHROUGH-DESIGN.md): zero-copy, no GPU, thin kernel.
 *  - Control path only crosses the CMD queue: small fixed messages, one
 *    request -> one response, mapping 1:1 to the VIDIOC_* ioctls c2.cix uses.
 *  - Buffers never cross the wire. On QBUF the guest sends the buffer's
 *    virtio-gpu RESOURCE ID (the minigbm/virtio-gpu blob behind the dmabuf);
 *    the host resolves it to host memory and queues it to mvx with
 *    V4L2_MEMORY_DMABUF, so the VPU DMAs the guest pages in place.
 *  - Completions (DQBUF) and events (SOURCE_CHANGE, EOS) come back on the EVT
 *    queue as used-buffer notifications: interrupt-driven, no guest polling.
 *
 * Little-endian wire (guest and host are both aarch64 LE). All structs packed.
 */
#ifndef TEBOX_V4L2_PROTO_H
#define TEBOX_V4L2_PROTO_H

#include <stdint.h>

/* Local, non-upstream virtio device id (an unused small id; NOT 48 =
 * VIRTIO_ID_MEDIA, to avoid a stock virtio-media driver binding this custom
 * protocol). Must match the guest driver. virtio_init() takes a uint16_t. */
#define TEBOX_V4L2_VIRTIO_ID 60

/* Virtqueues. */
enum {
    TEBOX_V4L2_VQ_CMD = 0,   /* guest -> host request, host fills response  */
    TEBOX_V4L2_VQ_EVT = 1,   /* host  -> guest async events / completions   */
    TEBOX_V4L2_VQ_MAX = 2,
};

/* One stream per host node; the guest opens one of each for c2.cix. */
enum {
    TEBOX_V4L2_NODE_DECODER = 0,  /* host /dev/video0 (mvxdec) */
    TEBOX_V4L2_NODE_ENCODER = 1,  /* host /dev/video1 (mvxenc) */
};

/* CMD opcodes: a minimal V4L2 M2M stateful subset. */
enum {
    TEBOX_V4L2_OP_QUERYCAP = 1,
    TEBOX_V4L2_OP_ENUM_FMT = 2,   /* by (buf_type, index)                 */
    TEBOX_V4L2_OP_S_FMT = 3,
    TEBOX_V4L2_OP_G_FMT = 4,
    TEBOX_V4L2_OP_REQBUFS = 5,
    TEBOX_V4L2_OP_QBUF = 6,       /* carries a resource id, not pixels     */
    TEBOX_V4L2_OP_STREAMON = 7,
    TEBOX_V4L2_OP_STREAMOFF = 8,
    TEBOX_V4L2_OP_S_CTRL = 9,
    TEBOX_V4L2_OP_G_CTRL = 10,
    TEBOX_V4L2_OP_SUBSCRIBE_EVENT = 11,
    TEBOX_V4L2_OP_OPEN = 12,      /* bind this stream to a host node       */
    TEBOX_V4L2_OP_CLOSE = 13,
    TEBOX_V4L2_OP_DQBUF = 14,     /* reclaim a finished buffer (data plane) */
    TEBOX_V4L2_OP_TRY_FMT = 15,   /* adjust a format to the node, no commit  */
    TEBOX_V4L2_OP_ENCODER_CMD = 16, /* VIDIOC_(TRY_)ENCODER_CMD (flush/drain) */
    TEBOX_V4L2_OP_IOCTL_TRACE = 17, /* diagnostic: log every guest ioctl cmd */
};

/* --- Data-plane transport convention (stage 3a) --------------------------
 * Buffer payloads ride as EXTRA scatter-gather entries on the CMD vq, after
 * the fixed header structs:
 *   OP_QBUF, dir=OUTPUT (coded in): out-sg = [cmd_hdr + tebox_v4l2_qbuf]
 *       [coded bytes, length = plane_bytesused[0]]; in-sg = [cmd_resp].
 *   OP_QBUF, dir=CAPTURE: out-sg = [cmd_hdr + tebox_v4l2_qbuf]; no data (the
 *       host just hands mvx a CAPTURE buffer). in-sg = [cmd_resp].
 *   OP_DQBUF, dir=CAPTURE (raw out): out-sg = [cmd_hdr + tebox_v4l2_dqbuf];
 *       in-sg = [cmd_resp + tebox_v4l2_dqbuf][NV12 bytes, capacity set by the
 *       guest]. resp.result = 0 with the filled tebox_v4l2_dqbuf, or -EAGAIN
 *       if no frame is ready yet, or -EPIPE at EOS.
 *   OP_DQBUF, dir=OUTPUT: returns the index of a consumed input buffer in
 *       tebox_v4l2_dqbuf (no data), or -EAGAIN.
 * Stage 3b replaces the inline bytes with a virtio-gpu resource id
 * (tebox_v4l2_qbuf.resource_id) that the host maps and feeds mvx via DMABUF.
 */

/* OP_DQBUF request (dir) and response payload. */
struct tebox_v4l2_dqbuf {
    uint32_t dir;
    uint32_t index;         /* buffer index that finished            */
    uint32_t num_planes;
    uint32_t plane_bytesused[4];
    uint64_t timestamp_us;  /* frame pts (copied from the matching QBUF) */
    uint32_t flags;         /* V4L2_BUF_FLAG_LAST / KEYFRAME / ERROR    */
    uint32_t reserved;
} __attribute__((packed));

/* Buffer queue direction (matches V4L2 OUTPUT/CAPTURE mplane roles). */
enum {
    TEBOX_V4L2_DIR_OUTPUT = 0,   /* guest -> VPU (coded for dec, raw for enc) */
    TEBOX_V4L2_DIR_CAPTURE = 1,  /* VPU  -> guest (raw for dec, coded for enc)*/
};

/* Every CMD request starts with this; |stream| selects the opened node. */
struct tebox_v4l2_cmd_hdr {
    uint32_t op;        /* TEBOX_V4L2_OP_*     */
    uint32_t stream;    /* per-open stream id  */
    uint32_t seq;       /* request serial      */
    uint32_t len;       /* bytes of payload that follow */
} __attribute__((packed));

/* Response header the host writes back on the CMD queue. */
struct tebox_v4l2_cmd_resp {
    uint32_t seq;       /* echoes the request  */
    int32_t  result;    /* 0 or -errno         */
    uint32_t len;       /* bytes of payload that follow */
    uint32_t reserved;
} __attribute__((packed));

/* OP_OPEN payload. */
struct tebox_v4l2_open {
    uint32_t node;      /* TEBOX_V4L2_NODE_*   */
} __attribute__((packed));

/* OP_S_FMT / OP_G_FMT payload (mplane, the only form mvx uses). */
struct tebox_v4l2_format {
    uint32_t dir;           /* TEBOX_V4L2_DIR_*            */
    uint32_t pixelformat;   /* V4L2 fourcc                */
    uint32_t width;
    uint32_t height;
    uint32_t num_planes;
    uint32_t plane_size[4]; /* sizeimage per plane        */
    uint32_t plane_stride[4];
} __attribute__((packed));

/* OP_ENUM_FMT request / response. */
struct tebox_v4l2_enum_fmt {
    uint32_t dir;
    uint32_t index;
    uint32_t pixelformat;   /* response: the fourcc, or 0 at end */
    uint32_t flags;
} __attribute__((packed));

/* OP_REQBUFS. */
struct tebox_v4l2_reqbufs {
    uint32_t dir;
    uint32_t count;         /* request: wanted; response: granted */
} __attribute__((packed));

/* OP_QBUF: the buffer is identified by its virtio-gpu resource id; the host
 * imports that as a dmabuf for mvx. No pixel data crosses the wire. */
struct tebox_v4l2_qbuf {
    uint32_t dir;
    uint32_t index;         /* V4L2 buffer index within the queue */
    uint32_t resource_id;   /* virtio-gpu resource behind the dmabuf */
    uint32_t num_planes;
    uint32_t plane_offset[4];
    uint32_t plane_bytesused[4];
    uint64_t timestamp_us;  /* carries pts; mvx copies it to the output */
    uint32_t flags;         /* V4L2_BUF_FLAG_* subset (KEYFRAME, LAST)   */
    uint32_t reserved;
} __attribute__((packed));

/* OP_STREAMON / OP_STREAMOFF / OP_SUBSCRIBE_EVENT payload. */
struct tebox_v4l2_dir_arg {
    uint32_t dir;
} __attribute__((packed));

/* OP_S_CTRL / OP_G_CTRL payload. */
struct tebox_v4l2_ctrl {
    uint32_t id;            /* V4L2_CID_*  */
    int32_t  value;
} __attribute__((packed));

/* OP_ENCODER_CMD: VIDIOC_ENCODER_CMD (is_try=0) or TRY_ENCODER_CMD (is_try=1).
 * The c2.cix encoder probes V4L2_ENC_CMD_STOP (flush) support right after open
 * and treats a failure as fatal, so the node must honour this. */
struct tebox_v4l2_encoder_cmd {
    uint32_t cmd;           /* V4L2_ENC_CMD_*  */
    uint32_t flags;         /* V4L2_ENC_CMD_STOP_AT_GOP_END, etc. */
    uint32_t is_try;        /* 1 = TRY (validate only), 0 = commit */
    uint32_t reserved;
} __attribute__((packed));

/* EVT queue: the host pushes these as buffers complete or state changes. */
enum {
    TEBOX_V4L2_EVT_DQBUF = 1,        /* a queued buffer finished */
    TEBOX_V4L2_EVT_SOURCE_CHANGE = 2,/* stateful decoder resolution change */
    TEBOX_V4L2_EVT_EOS = 3,
    TEBOX_V4L2_EVT_ERROR = 4,
};

struct tebox_v4l2_event {
    uint32_t type;          /* TEBOX_V4L2_EVT_*  */
    uint32_t stream;
    uint32_t dir;           /* for DQBUF: which queue completed  */
    uint32_t index;         /* for DQBUF: buffer index           */
    uint32_t num_planes;
    uint32_t plane_bytesused[4];
    uint64_t timestamp_us;  /* for DQBUF: the frame's pts        */
    uint32_t flags;         /* V4L2_BUF_FLAG_LAST / ERROR etc.   */
    uint32_t reserved;
} __attribute__((packed));

#endif /* TEBOX_V4L2_PROTO_H */
