/*
 * tebox-v4l2 (local, not for upstream): a virtio device that lets a guest
 * V4L2 M2M driver (tebox_v4l2.ko) drive the host MVX VPU V4L2 nodes
 * (/dev/video0 = mvxdec, /dev/video1 = mvxenc) so the stock CIX c2.cix Codec2
 * HAL works unmodified. Zero-copy, no GPU, thin kernel — see
 * thirdparty/vendor-drivers/cix/codec2/PASSTHROUGH-DESIGN.md and
 * tebox-v4l2-proto.h for the wire protocol.
 *
 * Stage 2 (this file, now): the control plane. The CMD virtqueue carries the
 * V4L2 ioctls c2.cix needs to negotiate formats (OPEN/ENUM_FMT/S_FMT/G_FMT/
 * REQBUFS); each is replayed on the real host node and the result returned, so
 * the guest driver can bring up /dev/video with the MVX format lists. The
 * data plane (QBUF/DQBUF via virtio-gpu resource ids, EVT queue) is stage 3.
 */
#include "qemu/osdep.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "qemu/module.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/pci/pci.h"
#include "qapi/error.h"
#include "qom/object.h"

#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>

#include "tebox-v4l2-proto.h"
#include "tebox-gpu-res.h"

/* Weak stub: virtio-gpu-rutabaga provides the real transfer_read. */
__attribute__((weak)) int tebox_gpu_resource_read_rgba(uint32_t resource_id,
        uint32_t width, uint32_t height, void *dst, size_t dst_bytes,
        uint32_t *stride_out)
{
    (void)resource_id;
    (void)width;
    (void)height;
    (void)dst;
    (void)dst_bytes;
    (void)stride_out;
    return -ENOSYS;
}

#define TYPE_TEBOX_V4L2 "tebox-v4l2"
OBJECT_DECLARE_SIMPLE_TYPE(TeboxV4l2, TEBOX_V4L2)

#define TEBOX_V4L2_MAX_STREAMS 8
#define TBX_MAX_BUFS 16   /* per queue (OUTPUT / CAPTURE) */

/* A host mvx MMAP buffer (coded on OUTPUT, NV12 on CAPTURE). */
typedef struct TeboxBuf {
    unsigned num_planes;
    void *ptr[VIDEO_MAX_PLANES];
    size_t length[VIDEO_MAX_PLANES];
    bool busy;              /* queued to mvx, not yet dequeued      */
    uint32_t guest_index;   /* OUTPUT: the guest buffer index it holds */
    uint64_t ts_us;         /* OUTPUT: pts carried to the frame      */
} TeboxBuf;

typedef struct TeboxStream {
    bool used;
    int node;      /* TEBOX_V4L2_NODE_* */
    int fd;        /* host /dev/videoN  */

    /* Data-plane streaming state (mvx V4L2 M2M, MMAP). OUTPUT carries the
     * coded fourcc, CAPTURE carries NV12. */
    bool events_subscribed;
    bool out_streaming, cap_streaming;
    bool capture_ready;     /* CAPTURE queue set up after SOURCE_CHANGE */
    bool saw_eos;
    unsigned out_n, cap_n;
    TeboxBuf out[TBX_MAX_BUFS];
    TeboxBuf cap[TBX_MAX_BUFS];
    struct v4l2_format cap_fmt;   /* NV12 geometry post source-change   */
} TeboxStream;

struct TeboxV4l2 {
    VirtIODevice parent_obj;
    VirtQueue *cmd_vq;
    VirtQueue *evt_vq;
    TeboxStream streams[TEBOX_V4L2_MAX_STREAMS];
};

static const char *const tebox_v4l2_node_path[] = {
    [TEBOX_V4L2_NODE_DECODER] = "/dev/video0",
    [TEBOX_V4L2_NODE_ENCODER] = "/dev/video1",
};

/* Encoder vs decoder swaps the queue roles: for the decoder OUTPUT carries the
 * coded stream in and CAPTURE the raw frames out (geometry learned via a
 * SOURCE_CHANGE event); for the encoder OUTPUT carries raw frames in and
 * CAPTURE the coded stream out, whose format is fixed at S_FMT with no
 * SOURCE_CHANGE, so CAPTURE must be brought up when the guest streams it on. */
static inline bool tbx_is_encoder(const TeboxStream *s)
{
    return s->node == TEBOX_V4L2_NODE_ENCODER;
}

/* Bring-up tracing for the encode data plane: QEMU's info_report (stderr) does
 * not reach `docker logs` here, so write to a file readable via docker exec. */
static void tbxv_log(const char *fmt, ...)
{
    static FILE *f;
    if (!f) {
        f = fopen("/tmp/tbxv4l2.log", "a");
        if (!f) {
            return;
        }
    }
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fflush(f);
}

static int tebox_v4l2_xioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

static uint32_t tebox_v4l2_buf_type(uint32_t dir)
{
    return dir == TEBOX_V4L2_DIR_OUTPUT ? V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE
                                        : V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
}

/* ---- host mvx V4L2 M2M data-plane helpers -------------------------------- */

static void tbx_unmap_queue(TeboxBuf *bufs, unsigned *n)
{
    for (unsigned i = 0; i < *n; i++) {
        for (unsigned p = 0; p < bufs[i].num_planes; p++) {
            if (bufs[i].ptr[p] && bufs[i].ptr[p] != MAP_FAILED) {
                munmap(bufs[i].ptr[p], bufs[i].length[p]);
            }
        }
        memset(&bufs[i], 0, sizeof(bufs[i]));
    }
    *n = 0;
}

/* Munmap + VIDIOC_REQBUFS(0) so the host mvx node is idle for a later S_FMT.
 * Without the REQBUFS(0), S_FMT returns EBUSY after the first encode session. */
static void tbx_free_queue(TeboxStream *s, uint32_t type, TeboxBuf *bufs, unsigned *n)
{
    tbx_unmap_queue(bufs, n);
    struct v4l2_requestbuffers rb = {
        .count = 0, .type = type, .memory = V4L2_MEMORY_MMAP,
    };
    tebox_v4l2_xioctl(s->fd, VIDIOC_REQBUFS, &rb);
}

/* REQBUFS MMAP |want| buffers of |type| and mmap each plane into |bufs|. */
static int tbx_map_queue(TeboxStream *s, uint32_t type, TeboxBuf *bufs, unsigned *n,
                         unsigned want)
{
    struct v4l2_requestbuffers rb = {
        .count = want, .type = type, .memory = V4L2_MEMORY_MMAP,
    };
    if (tebox_v4l2_xioctl(s->fd, VIDIOC_REQBUFS, &rb) || !rb.count) {
        return -errno;
    }
    unsigned got = rb.count < TBX_MAX_BUFS ? rb.count : TBX_MAX_BUFS;
    for (unsigned i = 0; i < got; i++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        struct v4l2_buffer buf = {
            .index = i, .type = type, .memory = V4L2_MEMORY_MMAP,
            .m.planes = planes, .length = VIDEO_MAX_PLANES,
        };
        memset(planes, 0, sizeof(planes));
        if (tebox_v4l2_xioctl(s->fd, VIDIOC_QUERYBUF, &buf)) {
            return -errno;
        }
        bufs[i].num_planes = buf.length;
        bufs[i].guest_index = UINT32_MAX;   /* no pending done-notification yet */
        for (unsigned p = 0; p < buf.length; p++) {
            bufs[i].length[p] = planes[p].length;
            bufs[i].ptr[p] = mmap(NULL, planes[p].length, PROT_READ | PROT_WRITE,
                                  MAP_SHARED, s->fd, planes[p].m.mem_offset);
            if (bufs[i].ptr[p] == MAP_FAILED) {
                return -errno;
            }
        }
    }
    *n = got;
    return 0;
}

static void tbx_subscribe(TeboxStream *s)
{
    if (s->events_subscribed) {
        return;
    }
    struct v4l2_event_subscription sub = { .type = V4L2_EVENT_SOURCE_CHANGE };
    tebox_v4l2_xioctl(s->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    sub.type = V4L2_EVENT_EOS;
    tebox_v4l2_xioctl(s->fd, VIDIOC_SUBSCRIBE_EVENT, &sub);
    s->events_subscribed = true;
}

static int tbx_qbuf_capture(TeboxStream *s, unsigned index)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(planes, 0, sizeof(planes));
    struct v4l2_buffer buf = {
        .index = index, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP, .m.planes = planes,
        .length = s->cap[index].num_planes,
    };
    for (unsigned p = 0; p < buf.length; p++) {
        planes[p].length = s->cap[index].length[p];
    }
    if (tebox_v4l2_xioctl(s->fd, VIDIOC_QBUF, &buf)) {
        return -errno;
    }
    s->cap[index].busy = true;
    return 0;
}

/* Stateful decoder: after the driver absorbs enough coded data it raises
 * V4L2_EVENT_SOURCE_CHANGE; then the CAPTURE geometry (NV12 dims) is known and
 * we allocate + queue + stream the CAPTURE side. Host-managed: the guest's
 * CAPTURE buffers are just NV12 sinks, decoupled from mvx CAPTURE buffers. */
static int tbx_setup_capture(TeboxStream *s)
{
    if (s->capture_ready) {
        return 0;
    }
    tbxv_log("setup_capture: node=%d enc=%d begin", s->node, tbx_is_encoder(s));
    memset(&s->cap_fmt, 0, sizeof(s->cap_fmt));
    s->cap_fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (tebox_v4l2_xioctl(s->fd, VIDIOC_G_FMT, &s->cap_fmt)) {
        tbxv_log("setup_capture: G_FMT CAPTURE failed errno=%d", errno);
        return -errno;
    }
    tbxv_log("setup_capture: CAPTURE fmt %ux%u fourcc=0x%08x planes=%u",
             s->cap_fmt.fmt.pix_mp.width, s->cap_fmt.fmt.pix_mp.height,
             s->cap_fmt.fmt.pix_mp.pixelformat, s->cap_fmt.fmt.pix_mp.num_planes);
    int ret = tbx_map_queue(s, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, s->cap, &s->cap_n,
                            TBX_MAX_BUFS);
    if (ret) {
        tbxv_log("setup_capture: map_queue CAPTURE failed ret=%d", ret);
        return ret;
    }
    for (unsigned i = 0; i < s->cap_n; i++) {
        ret = tbx_qbuf_capture(s, i);
        if (ret) {
            tbxv_log("setup_capture: qbuf_capture %u failed ret=%d", i, ret);
            return ret;
        }
    }
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (tebox_v4l2_xioctl(s->fd, VIDIOC_STREAMON, &type)) {
        tbxv_log("setup_capture: STREAMON CAPTURE failed errno=%d", errno);
        return -errno;
    }
    tbxv_log("setup_capture: node=%d CAPTURE ready n=%u", s->node, s->cap_n);
    s->cap_streaming = true;
    s->capture_ready = true;
    info_report("tebox-v4l2: stream %d capture ready %ux%u",
                s->node, s->cap_fmt.fmt.pix_mp.width, s->cap_fmt.fmt.pix_mp.height);
    return 0;
}

/* Drain pending V4L2 events; set up CAPTURE on SOURCE_CHANGE, note EOS. */
static void tbx_pump_events(TeboxStream *s)
{
    struct v4l2_event ev;
    while (!tebox_v4l2_xioctl(s->fd, VIDIOC_DQEVENT, &ev)) {
        if (ev.type == V4L2_EVENT_SOURCE_CHANGE &&
            (ev.u.src_change.changes & V4L2_EVENT_SRC_CH_RESOLUTION)) {
            tbx_setup_capture(s);
        } else if (ev.type == V4L2_EVENT_EOS) {
            s->saw_eos = true;
        }
    }
}

/* Reclaim finished OUTPUT buffers so their slots free up.
 *
 * Guest device_run is sync 1:1 (QBUF OUTPUT → DQBUF CAPTURE) and marks the
 * guest src buffer DONE itself — it never issues OP_DQBUF OUTPUT. Clear
 * guest_index here too; otherwise after out_n frames every slot stays
 * guest_index!=UINT32_MAX and QBUF returns -EAGAIN forever (scrcpy stalls
 * at exactly REQBUFS(OUTPUT) frames, typically 2). */
static void tbx_reclaim_output(TeboxStream *s)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = V4L2_MEMORY_MMAP,
        .m.planes = planes, .length = VIDEO_MAX_PLANES,
    };
    while (!tebox_v4l2_xioctl(s->fd, VIDIOC_DQBUF, &buf)) {
        if (buf.index < s->out_n) {
            s->out[buf.index].busy = false;
            s->out[buf.index].guest_index = UINT32_MAX;
        }
        memset(planes, 0, sizeof(planes));
        buf.m.planes = planes;
        buf.length = VIDEO_MAX_PLANES;
    }
}

static void tbx_stop(TeboxStream *s)
{
    int type;
    if (s->out_streaming) {
        type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        tebox_v4l2_xioctl(s->fd, VIDIOC_STREAMOFF, &type);
        s->out_streaming = false;
    }
    if (s->cap_streaming) {
        type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        tebox_v4l2_xioctl(s->fd, VIDIOC_STREAMOFF, &type);
        s->cap_streaming = false;
    }
    tbx_free_queue(s, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, s->out, &s->out_n);
    tbx_free_queue(s, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, s->cap, &s->cap_n);
    s->capture_ready = false;
    s->saw_eos = false;
}

/* ---- control-plane op handlers: return 0 or -errno, fill *resp payload ---- */

static int tebox_v4l2_op_open(TeboxV4l2 *v, const void *in, uint32_t in_len,
                              void *out, uint32_t *out_len)
{
    if (in_len < sizeof(struct tebox_v4l2_open)) {
        return -EINVAL;
    }
    const struct tebox_v4l2_open *req = in;
    if (req->node > TEBOX_V4L2_NODE_ENCODER) {
        return -EINVAL;
    }
    int idx = -1;
    for (int i = 0; i < TEBOX_V4L2_MAX_STREAMS; i++) {
        if (!v->streams[i].used) { idx = i; break; }
    }
    if (idx < 0) {
        return -ENOSPC;
    }
    int fd = open(tebox_v4l2_node_path[req->node], O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fd < 0) {
        return -errno;
    }
    v->streams[idx].used = true;
    v->streams[idx].node = req->node;
    v->streams[idx].fd = fd;
    /* response: the assigned stream id */
    uint32_t *sid = out;
    *sid = (uint32_t)idx;
    *out_len = sizeof(*sid);
    info_report("tebox-v4l2: stream %d open node %u (%s)", idx, req->node,
                tebox_v4l2_node_path[req->node]);
    return 0;
}

static TeboxStream *tebox_v4l2_stream(TeboxV4l2 *v, uint32_t id)
{
    if (id >= TEBOX_V4L2_MAX_STREAMS || !v->streams[id].used) {
        return NULL;
    }
    return &v->streams[id];
}

static int tebox_v4l2_op_close(TeboxV4l2 *v, uint32_t stream)
{
    TeboxStream *s = tebox_v4l2_stream(v, stream);
    if (!s) {
        return -EBADF;
    }
    tbx_stop(s);
    close(s->fd);
    memset(s, 0, sizeof(*s));
    return 0;
}

static int tebox_v4l2_op_enum_fmt(TeboxStream *s, const void *in, uint32_t in_len,
                                  void *out, uint32_t *out_len)
{
    if (in_len < sizeof(struct tebox_v4l2_enum_fmt)) {
        return -EINVAL;
    }
    const struct tebox_v4l2_enum_fmt *req = in;
    struct v4l2_fmtdesc desc;
    memset(&desc, 0, sizeof(desc));
    desc.index = req->index;
    desc.type = tebox_v4l2_buf_type(req->dir);
    struct tebox_v4l2_enum_fmt *rsp = out;
    memset(rsp, 0, sizeof(*rsp));
    rsp->dir = req->dir;
    rsp->index = req->index;
    if (tebox_v4l2_xioctl(s->fd, VIDIOC_ENUM_FMT, &desc) < 0) {
        /* end of list: pixelformat 0 tells the guest to stop */
        if (s->node == TEBOX_V4L2_NODE_ENCODER) {
            tbxv_log("ENUM_FMT dir=%u idx=%u node=%d END", req->dir, req->index, s->node);
        }
        *out_len = sizeof(*rsp);
        return 0;
    }
    rsp->pixelformat = desc.pixelformat;
    rsp->flags = desc.flags;
    if (s->node == TEBOX_V4L2_NODE_ENCODER) {
        tbxv_log("ENUM_FMT dir=%u idx=%u node=%d -> fourcc=0x%08x '%c%c%c%c' flags=0x%x",
                 req->dir, req->index, s->node, desc.pixelformat,
                 desc.pixelformat & 0xff, (desc.pixelformat >> 8) & 0xff,
                 (desc.pixelformat >> 16) & 0xff, (desc.pixelformat >> 24) & 0xff,
                 desc.flags);
    }
    *out_len = sizeof(*rsp);
    return 0;
}

static int tebox_v4l2_op_set_get_fmt(TeboxStream *s, uint32_t op, const void *in,
                                     uint32_t in_len, void *out, uint32_t *out_len)
{
    if (in_len < sizeof(struct tebox_v4l2_format)) {
        return -EINVAL;
    }
    const struct tebox_v4l2_format *req = in;
    struct v4l2_format fmt;
    memset(&fmt, 0, sizeof(fmt));
    fmt.type = tebox_v4l2_buf_type(req->dir);
    /* S_FMT and TRY_FMT both carry the requested geometry; G_FMT just reads. */
    if (op == TEBOX_V4L2_OP_S_FMT || op == TEBOX_V4L2_OP_TRY_FMT) {
        fmt.fmt.pix_mp.pixelformat = req->pixelformat;
        fmt.fmt.pix_mp.width = req->width;
        fmt.fmt.pix_mp.height = req->height;
        fmt.fmt.pix_mp.num_planes = req->num_planes ? req->num_planes : 1;
        for (uint32_t p = 0; p < fmt.fmt.pix_mp.num_planes && p < VIDEO_MAX_PLANES; p++) {
            fmt.fmt.pix_mp.plane_fmt[p].sizeimage = req->plane_size[p < 4 ? p : 3];
            fmt.fmt.pix_mp.plane_fmt[p].bytesperline = req->plane_stride[p < 4 ? p : 3];
        }
    }
    /* S_FMT while host still holds MMAP buffers → EBUSY. Drop queues first. */
    if (op == TEBOX_V4L2_OP_S_FMT &&
        (s->out_n || s->cap_n || s->out_streaming || s->cap_streaming)) {
        tbxv_log("S_FMT: draining busy queues node=%d out=%u cap=%u",
                 s->node, s->out_n, s->cap_n);
        tbx_stop(s);
    }
    unsigned long ioc = op == TEBOX_V4L2_OP_S_FMT ? VIDIOC_S_FMT
                      : op == TEBOX_V4L2_OP_TRY_FMT ? VIDIOC_TRY_FMT
                      : VIDIOC_G_FMT;
    if (tebox_v4l2_xioctl(s->fd, ioc, &fmt) < 0) {
        tbxv_log("%s dir=%u node=%d req %ux%u fourcc=0x%08x FAILED errno=%d",
                 op == TEBOX_V4L2_OP_S_FMT ? "S_FMT" : op == TEBOX_V4L2_OP_TRY_FMT ? "TRY_FMT" : "G_FMT", req->dir, s->node,
                 req->width, req->height, req->pixelformat, errno);
        return -errno;
    }
    /* mvx sometimes leaves sizeimage=0 on coded CAPTURE; fill a bitstream
     * budget so the guest QUERYBUF reports a usable length to c2.cix. */
    for (uint32_t p = 0; p < fmt.fmt.pix_mp.num_planes && p < VIDEO_MAX_PLANES; p++) {
        if (!fmt.fmt.pix_mp.plane_fmt[p].sizeimage) {
            uint32_t wh = fmt.fmt.pix_mp.width * fmt.fmt.pix_mp.height;
            fmt.fmt.pix_mp.plane_fmt[p].sizeimage = wh > 0x100000 ? wh : 0x100000;
        }
    }
    tbxv_log("%s dir=%u node=%d req %ux%u -> %ux%u fourcc=0x%08x planes=%u sz0=%u",
             op == TEBOX_V4L2_OP_S_FMT ? "S_FMT" : op == TEBOX_V4L2_OP_TRY_FMT ? "TRY_FMT" : "G_FMT", req->dir, s->node,
             req->width, req->height,
             fmt.fmt.pix_mp.width, fmt.fmt.pix_mp.height,
             fmt.fmt.pix_mp.pixelformat, fmt.fmt.pix_mp.num_planes,
             fmt.fmt.pix_mp.plane_fmt[0].sizeimage);
    struct tebox_v4l2_format *rsp = out;
    memset(rsp, 0, sizeof(*rsp));
    rsp->dir = req->dir;
    rsp->pixelformat = fmt.fmt.pix_mp.pixelformat;
    rsp->width = fmt.fmt.pix_mp.width;
    rsp->height = fmt.fmt.pix_mp.height;
    rsp->num_planes = fmt.fmt.pix_mp.num_planes;
    for (uint32_t p = 0; p < fmt.fmt.pix_mp.num_planes && p < 4; p++) {
        rsp->plane_size[p] = fmt.fmt.pix_mp.plane_fmt[p].sizeimage;
        rsp->plane_stride[p] = fmt.fmt.pix_mp.plane_fmt[p].bytesperline;
    }
    *out_len = sizeof(*rsp);
    return 0;
}

static int tebox_v4l2_op_ctrl(TeboxStream *s, uint32_t op, const void *in,
                              uint32_t in_len, void *out, uint32_t *out_len)
{
    if (in_len < sizeof(struct tebox_v4l2_ctrl)) {
        return -EINVAL;
    }
    const struct tebox_v4l2_ctrl *req = in;
    struct v4l2_control c = { .id = req->id, .value = req->value };
    unsigned long ioc = op == TEBOX_V4L2_OP_S_CTRL ? VIDIOC_S_CTRL : VIDIOC_G_CTRL;
    int ret = tebox_v4l2_xioctl(s->fd, ioc, &c) < 0 ? -errno : 0;
    struct tebox_v4l2_ctrl *rsp = out;
    rsp->id = req->id;
    rsp->value = c.value;
    *out_len = sizeof(*rsp);
    tbxv_log("%s node=%d id=0x%x val=%d -> %d",
             op == TEBOX_V4L2_OP_S_CTRL ? "S_CTRL" : "G_CTRL",
             s->node, req->id, c.value, ret);
    return ret;
}

static int tebox_v4l2_op_encoder_cmd(TeboxStream *s, const void *in,
                                     uint32_t in_len, uint32_t *out_len)
{
    if (in_len < sizeof(struct tebox_v4l2_encoder_cmd)) {
        return -EINVAL;
    }
    const struct tebox_v4l2_encoder_cmd *req = in;
    struct v4l2_encoder_cmd ec;
    memset(&ec, 0, sizeof(ec));
    ec.cmd = req->cmd;
    ec.flags = req->flags;
    unsigned long ioc = req->is_try ? VIDIOC_TRY_ENCODER_CMD : VIDIOC_ENCODER_CMD;
    int ret = tebox_v4l2_xioctl(s->fd, ioc, &ec) < 0 ? -errno : 0;
    tbxv_log("ENCODER_CMD node=%d cmd=%u flags=%u try=%u -> %d",
             s->node, req->cmd, req->flags, req->is_try, ret);
    *out_len = 0;
    return ret;
}

static int tebox_v4l2_op_reqbufs(TeboxStream *s, const void *in, uint32_t in_len,
                                 void *out, uint32_t *out_len)
{
    if (in_len < sizeof(struct tebox_v4l2_reqbufs)) {
        return -EINVAL;
    }
    const struct tebox_v4l2_reqbufs *req = in;
    struct tebox_v4l2_reqbufs *rsp = out;
    rsp->dir = req->dir;
    *out_len = sizeof(*rsp);

    tbx_subscribe(s);
    tbxv_log("REQBUFS dir=%u node=%d count=%u", req->dir, s->node, req->count);
    if (req->count == 0) {
        /* Full teardown: STREAMOFF + REQBUFS(0) so the next S_FMT is not EBUSY. */
        if (req->dir == TEBOX_V4L2_DIR_OUTPUT) {
            if (s->out_streaming) {
                int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
                tebox_v4l2_xioctl(s->fd, VIDIOC_STREAMOFF, &type);
                s->out_streaming = false;
            }
            tbx_free_queue(s, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, s->out, &s->out_n);
        } else {
            if (s->cap_streaming) {
                int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
                tebox_v4l2_xioctl(s->fd, VIDIOC_STREAMOFF, &type);
                s->cap_streaming = false;
            }
            tbx_free_queue(s, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, s->cap, &s->cap_n);
            s->capture_ready = false;
        }
        rsp->count = 0;
        return 0;
    }
    if (req->dir == TEBOX_V4L2_DIR_OUTPUT) {
        /* Real mvx OUTPUT buffers (raw input for enc / coded for dec), MMAP. */
        tbx_free_queue(s, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, s->out, &s->out_n);
        int ret = tbx_map_queue(s, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, s->out,
                                &s->out_n, req->count);
        if (ret) {
            tbxv_log("REQBUFS OUTPUT map_queue failed ret=%d", ret);
            return ret;
        }
        rsp->count = s->out_n;
    } else {
        /* CAPTURE is host-managed (allocated on STREAMON/SOURCE_CHANGE for
         * enc/dec). Guest REQBUFS just reserves slots; echo the count. A
         * later STREAMOFF/REQBUFS(0) path above frees the host CAPTURE queue. */
        rsp->count = req->count;
    }
    return 0;
}

static int tebox_v4l2_op_streamon(TeboxStream *s, uint32_t dir, bool on)
{
    tbxv_log("STREAM%s dir=%u node=%d enc=%d", on ? "ON" : "OFF", dir, s->node,
             tbx_is_encoder(s));
    if (dir == TEBOX_V4L2_DIR_OUTPUT) {
        int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        if (tebox_v4l2_xioctl(s->fd, on ? VIDIOC_STREAMON : VIDIOC_STREAMOFF, &type)) {
            tbxv_log("STREAMON OUTPUT node=%d failed errno=%d", s->node, errno);
            return -errno;
        }
        s->out_streaming = on;
        if (!on) {
            tbx_free_queue(s, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, s->out, &s->out_n);
        }
    } else if (on) {
        /* Encoder: the coded CAPTURE format is already fixed by S_FMT, so bring
         * the queue up now (map + qbuf + streamon mvx CAPTURE) rather than
         * waiting for a SOURCE_CHANGE that only the stateful decoder raises. */
        if (tbx_is_encoder(s)) {
            int ret = tbx_setup_capture(s);
            if (ret) {
                return ret;
            }
        }
        /* Decoder: CAPTURE is driven host-side at SOURCE_CHANGE; nothing yet. */
    } else {
        /* STREAMOFF CAPTURE (either direction): tear the mvx CAPTURE queue down. */
        if (s->cap_streaming) {
            int type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
            tebox_v4l2_xioctl(s->fd, VIDIOC_STREAMOFF, &type);
            s->cap_streaming = false;
            s->capture_ready = false;
            tbx_free_queue(s, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, s->cap, &s->cap_n);
        }
    }
    return 0;
}

/* OP_QBUF: has the whole vq element so OUTPUT can read the coded bytes that
 * follow the header structs in out_sg. */
static int tebox_v4l2_op_qbuf(TeboxStream *s, const struct tebox_v4l2_cmd_hdr *hdr,
                              const struct tebox_v4l2_qbuf *q, VirtQueueElement *elem)
{
    tbxv_log("QBUF dir=%u node=%d np=%u used0=%u res=%u", q->dir, s->node,
             q->num_planes, q->plane_bytesused[0], q->resource_id);
    if (q->dir == TEBOX_V4L2_DIR_CAPTURE) {
        /* Guest signals a free NV12 sink; real mvx CAPTURE bufs are recycled on
         * DQBUF, so nothing to do until the decoder produces frames. */
        tbx_pump_events(s);
        return 0;
    }

    /* OUTPUT: copy the coded access unit into a free mvx OUTPUT buffer. A slot
     * is free only once mvx consumed it AND the guest dequeued the done index. */
    tbx_reclaim_output(s);
    unsigned idx = s->out_n;
    for (unsigned i = 0; i < s->out_n; i++) {
        if (!s->out[i].busy && s->out[i].guest_index == UINT32_MAX) { idx = i; break; }
    }
    if (idx == s->out_n) {
        return -EAGAIN;  /* no free input buffer; guest retries */
    }
    /* Copy each plane the guest sent (coded is one plane; an encoder's raw
     * OUTPUT frame may be multi-plane, e.g. NV12 as separate Y/UV planes) into
     * the matching mvx OUTPUT buffer plane. Surface/AB24 frames send
     * resource_id and rely on virtio-gpu transfer_read — guest vmap is zeros. */
    unsigned np = q->num_planes ? q->num_planes : 1;
    if (np > s->out[idx].num_planes) {
        np = s->out[idx].num_planes;
    }
    if (np > VIDEO_MAX_PLANES) {
        np = VIDEO_MAX_PLANES;
    }
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(planes, 0, sizeof(planes));
    size_t off = sizeof(*hdr) + sizeof(*q);
    if (q->resource_id && tbx_is_encoder(s) && np == 1) {
        struct v4l2_format fmt = { .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE };
        uint32_t w = 0, h = 0, stride = 0;
        uint32_t len = q->plane_bytesused[0];
        int rr;

        if (!tebox_v4l2_xioctl(s->fd, VIDIOC_G_FMT, &fmt)) {
            w = fmt.fmt.pix_mp.width;
            h = fmt.fmt.pix_mp.height;
        }
        if (!len) {
            len = w * h * 4;
        }
        if (len > s->out[idx].length[0]) {
            return -EMSGSIZE;
        }
        rr = tebox_gpu_resource_read_rgba(q->resource_id, w, h,
                                          s->out[idx].ptr[0], len, &stride);
        tbxv_log("QBUF res=%u %ux%u transfer_read -> %d stride=%u",
                 q->resource_id, w, h, rr, stride);
        if (rr) {
            return rr;
        }
        planes[0].bytesused = len;
        planes[0].length = s->out[idx].length[0];
    } else {
        for (unsigned p = 0; p < np; p++) {
            uint32_t len = q->plane_bytesused[p < 4 ? p : 3];
            if (len > s->out[idx].length[p]) {
                return -EMSGSIZE;
            }
            if (len) {
                if (iov_to_buf(elem->out_sg, elem->out_num, off,
                               s->out[idx].ptr[p], len) != len) {
                    return -EIO;
                }
                off += len;
            }
            planes[p].bytesused = len;
            planes[p].length = s->out[idx].length[p];
        }
    }
    struct v4l2_buffer buf = {
        .index = idx, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
        .memory = V4L2_MEMORY_MMAP, .m.planes = planes, .length = np,
        .timestamp.tv_sec = q->timestamp_us / 1000000,
        .timestamp.tv_usec = q->timestamp_us % 1000000,
    };
    if (q->flags & V4L2_BUF_FLAG_LAST) {
        buf.flags |= V4L2_BUF_FLAG_LAST;  /* carry EOS marker if the guest set it */
    }
    if (tebox_v4l2_xioctl(s->fd, VIDIOC_QBUF, &buf)) {
        tbxv_log("QBUF OUTPUT node=%d idx=%u np=%u failed errno=%d", s->node, idx, np, errno);
        return -errno;
    }
    s->out[idx].busy = true;
    s->out[idx].guest_index = q->index;
    tbx_pump_events(s);   /* a source change may be ready now */
    return 0;
}

/* Bounded wait for the stateful decoder. mvx raises SOURCE_CHANGE and produces
 * the first decoded frame asynchronously, a few ms after it absorbs the opening
 * coded buffers; a single non-blocking DQEVENT/DQBUF probe almost always misses
 * that window and the guest's synchronous 1:1 device_run then completes an empty
 * CAPTURE buffer (bytesused 0). Here we pump events (which sets up CAPTURE on
 * SOURCE_CHANGE) and poll the mvx fd until a frame is dequeuable, EOS is seen,
 * or the deadline passes. The guest issued OP_DQBUF and is already blocked on
 * the response, so briefly blocking this request matches the 1:1 flow.
 * Returns 1 if a CAPTURE frame is ready to DQBUF, 0 on timeout, <0 at EOS. */
#define TBX_DQBUF_WAIT_US 120000  /* covers SOURCE_CHANGE + first-frame latency */
static int tbx_wait_capture(TeboxStream *s)
{
    int64_t deadline = g_get_monotonic_time() + TBX_DQBUF_WAIT_US;
    for (;;) {
        tbx_pump_events(s);
        if (s->capture_ready) {
            struct pollfd pfd = { .fd = s->fd, .events = POLLIN | POLLPRI };
            if (poll(&pfd, 1, 0) > 0 && (pfd.revents & POLLIN)) {
                return 1;  /* a decoded frame is queued */
            }
        }
        if (s->saw_eos) {
            return -1;
        }
        int64_t remain_us = deadline - g_get_monotonic_time();
        if (remain_us <= 0) {
            return 0;
        }
        /* Wake on POLLPRI (SOURCE_CHANGE/EOS event) or POLLIN (a decoded frame).
         * Before CAPTURE streamon only POLLPRI can fire; after it, POLLIN. */
        struct pollfd pfd = { .fd = s->fd, .events = POLLIN | POLLPRI };
        poll(&pfd, 1, (int)((remain_us + 999) / 1000));
    }
}

/* OP_DQBUF: CAPTURE returns a decoded NV12 frame (data in in_sg after the
 * headers); OUTPUT returns a consumed input index. */
static int tebox_v4l2_op_dqbuf(TeboxStream *s, const struct tebox_v4l2_dqbuf *req,
                               VirtQueueElement *elem, uint32_t *out_len)
{
    struct tebox_v4l2_dqbuf rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.dir = req->dir;

    if (req->dir == TEBOX_V4L2_DIR_OUTPUT) {
        tbx_reclaim_output(s);
        for (unsigned i = 0; i < s->out_n; i++) {
            if (!s->out[i].busy && s->out[i].guest_index != UINT32_MAX) {
                rsp.index = s->out[i].guest_index;
                s->out[i].guest_index = UINT32_MAX;
                iov_from_buf(elem->in_sg, elem->in_num, sizeof(struct tebox_v4l2_cmd_resp),
                             &rsp, sizeof(rsp));
                *out_len = sizeof(rsp);
                return 0;
            }
        }
        return -EAGAIN;
    }

    /* CAPTURE: bounded-wait for a coded frame (encoder) or a decoded NV12 frame
     * (decoder; also sets up CAPTURE on SOURCE_CHANGE), then DQBUF it. */
    int w = tbx_wait_capture(s);
    tbxv_log("DQBUF CAPTURE node=%d enc=%d wait=%d cap_ready=%d",
             s->node, tbx_is_encoder(s), w, s->capture_ready);
    if (w <= 0) {
        return (w < 0 || s->saw_eos) ? -EPIPE : -EAGAIN;
    }
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    memset(planes, 0, sizeof(planes));
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
        .m.planes = planes, .length = VIDEO_MAX_PLANES,
    };
    if (tebox_v4l2_xioctl(s->fd, VIDIOC_DQBUF, &buf)) {
        if (errno == EAGAIN) {
            return s->saw_eos ? -EPIPE : -EAGAIN;
        }
        if (errno == EPIPE) {
            s->saw_eos = true;
            return -EPIPE;
        }
        return -errno;
    }
    /* Copy the decoded planes into the guest's in_sg after resp + dqbuf hdr. */
    unsigned np = s->cap[buf.index].num_planes;
    size_t off = sizeof(struct tebox_v4l2_cmd_resp) + sizeof(rsp);
    rsp.num_planes = np;
    rsp.timestamp_us = (uint64_t)buf.timestamp.tv_sec * 1000000 + buf.timestamp.tv_usec;
    if (buf.flags & V4L2_BUF_FLAG_LAST) {
        rsp.flags |= V4L2_BUF_FLAG_LAST;
        s->saw_eos = true;
    }
    for (unsigned p = 0; p < np; p++) {
        uint32_t used = planes[p].bytesused;
        rsp.plane_bytesused[p] = used;
        if (used) {
            off += iov_from_buf(elem->in_sg, elem->in_num, off, s->cap[buf.index].ptr[p], used);
        }
    }
    iov_from_buf(elem->in_sg, elem->in_num, sizeof(struct tebox_v4l2_cmd_resp),
                 &rsp, sizeof(rsp));
    *out_len = (uint32_t)(off - sizeof(struct tebox_v4l2_cmd_resp));
    /* Recycle this mvx CAPTURE buffer for the next frame. */
    tbx_qbuf_capture(s, buf.index);
    /* Free OUTPUT slots whose encode finished while we waited on CAPTURE. */
    tbx_reclaim_output(s);
    return 0;
}

/* Dispatch one CMD request; returns result and the response payload. */
static int tebox_v4l2_dispatch(TeboxV4l2 *v, const struct tebox_v4l2_cmd_hdr *hdr,
                               const void *in, void *out, uint32_t *out_len)
{
    *out_len = 0;
    if (hdr->op == TEBOX_V4L2_OP_OPEN) {
        return tebox_v4l2_op_open(v, in, hdr->len, out, out_len);
    }
    if (hdr->op == TEBOX_V4L2_OP_CLOSE) {
        return tebox_v4l2_op_close(v, hdr->stream);
    }
    TeboxStream *s = tebox_v4l2_stream(v, hdr->stream);
    if (!s) {
        return -EBADF;
    }
    if (s->node == TEBOX_V4L2_NODE_ENCODER) {
        tbxv_log("OP %u stream=%u node=%d len=%u", hdr->op, hdr->stream, s->node, hdr->len);
    }
    switch (hdr->op) {
    case TEBOX_V4L2_OP_QUERYCAP:
        return 0;  /* the guest driver reports its own caps */
    case TEBOX_V4L2_OP_ENUM_FMT:
        return tebox_v4l2_op_enum_fmt(s, in, hdr->len, out, out_len);
    case TEBOX_V4L2_OP_S_FMT:
    case TEBOX_V4L2_OP_G_FMT:
    case TEBOX_V4L2_OP_TRY_FMT:
        return tebox_v4l2_op_set_get_fmt(s, hdr->op, in, hdr->len, out, out_len);
    case TEBOX_V4L2_OP_REQBUFS:
        return tebox_v4l2_op_reqbufs(s, in, hdr->len, out, out_len);
    case TEBOX_V4L2_OP_STREAMON:
    case TEBOX_V4L2_OP_STREAMOFF:
        if (hdr->len < sizeof(struct tebox_v4l2_dir_arg)) {
            return -EINVAL;
        }
        return tebox_v4l2_op_streamon(s, ((const struct tebox_v4l2_dir_arg *)in)->dir,
                                      hdr->op == TEBOX_V4L2_OP_STREAMON);
    case TEBOX_V4L2_OP_IOCTL_TRACE: {
        if (hdr->len >= 12 && ((const uint32_t *)in)[0] == 0x52455142) {
            const uint32_t *p = in;  /* "REQB": type, memory */
            tbxv_log("REQBUFS-try node=%d type=%u memory=%u", s->node, p[1], p[2]);
            *out_len = 0;
            return 0;
        }
        if (hdr->len >= 8) {
            const uint32_t *p = in;  /* cmd + errno: an ioctl that failed */
            tbxv_log("IOCTL node=%d cmd=0x%08x nr=%u FAILED ret=%d",
                     s->node, p[0], (p[0] >> 0) & 0xff, (int32_t)p[1]);
        } else if (hdr->len >= 4) {
            const uint32_t *p = in;
            tbxv_log("IOCTL node=%d cmd=0x%08x nr=%u", s->node, p[0], p[0] & 0xff);
        }
        *out_len = 0;
        return 0;
    }
    case TEBOX_V4L2_OP_ENCODER_CMD:
        return tebox_v4l2_op_encoder_cmd(s, in, hdr->len, out_len);
    case TEBOX_V4L2_OP_SUBSCRIBE_EVENT:
        tbx_subscribe(s);
        return 0;
    case TEBOX_V4L2_OP_S_CTRL:
    case TEBOX_V4L2_OP_G_CTRL:
        return tebox_v4l2_op_ctrl(s, hdr->op, in, hdr->len, out, out_len);
    default:
        return -EINVAL;  /* QBUF/DQBUF handled in the CMD handler (need elem) */
    }
}

static void tebox_v4l2_handle_cmd(VirtIODevice *vdev, VirtQueue *vq)
{
    TeboxV4l2 *v = TEBOX_V4L2(vdev);
    for (;;) {
        VirtQueueElement *elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            break;
        }
        struct tebox_v4l2_cmd_hdr hdr;
        g_autofree uint8_t *inbuf = NULL;
        g_autofree uint8_t *outbuf = NULL;
        struct tebox_v4l2_cmd_resp resp = { 0 };
        uint32_t payload_len = 0;

        if (iov_to_buf(elem->out_sg, elem->out_num, 0, &hdr, sizeof(hdr)) !=
            sizeof(hdr)) {
            resp.result = -EINVAL;
            goto reply;
        }
        resp.seq = hdr.seq;

        /* Data-plane ops carry bulk buffer bytes as extra sg entries, so they
         * work on the whole element rather than the copied small payload. */
        if (hdr.op == TEBOX_V4L2_OP_QBUF || hdr.op == TEBOX_V4L2_OP_DQBUF) {
            TeboxStream *s = tebox_v4l2_stream(v, hdr.stream);
            if (!s) {
                resp.result = -EBADF;
                goto reply;
            }
            if (hdr.op == TEBOX_V4L2_OP_QBUF) {
                struct tebox_v4l2_qbuf q;
                if (iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), &q, sizeof(q)) !=
                    sizeof(q)) {
                    resp.result = -EINVAL;
                    goto reply;
                }
                resp.result = tebox_v4l2_op_qbuf(s, &hdr, &q, elem);
            } else {
                struct tebox_v4l2_dqbuf dq;
                if (iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), &dq, sizeof(dq)) !=
                    sizeof(dq)) {
                    resp.result = -EINVAL;
                    goto reply;
                }
                resp.result = tebox_v4l2_op_dqbuf(s, &dq, elem, &payload_len);
            }
            resp.len = payload_len;
            /* op_dqbuf already wrote its payload into in_sg after the resp. */
            iov_from_buf(elem->in_sg, elem->in_num, 0, &resp, sizeof(resp));
            virtqueue_push(vq, elem, sizeof(resp) + payload_len);
            g_free(elem);
            continue;
        }

        if (hdr.len > (64u << 10)) {
            resp.result = -EMSGSIZE;
            goto reply;
        }
        inbuf = g_malloc0(hdr.len ? hdr.len : 1);
        if (hdr.len) {
            iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), inbuf, hdr.len);
        }
        /* Response payloads are small, fixed structs; one page is plenty. */
        outbuf = g_malloc0(4096);
        resp.result = tebox_v4l2_dispatch(v, &hdr, inbuf, outbuf, &payload_len);
        resp.len = payload_len;

    reply:
        {
            size_t off = iov_from_buf(elem->in_sg, elem->in_num, 0, &resp,
                                      sizeof(resp));
            if (payload_len && outbuf) {
                iov_from_buf(elem->in_sg, elem->in_num, off, outbuf, payload_len);
            }
            virtqueue_push(vq, elem, sizeof(resp) + payload_len);
        }
        g_free(elem);
    }
    virtio_notify(vdev, vq);
}

/* EVT queue: the guest posts receive buffers here; the host fills them when a
 * buffer completes or the decoder signals SOURCE_CHANGE. Nothing is pushed in
 * the control-only stage; the handler just absorbs the kick. */
static void tebox_v4l2_handle_evt(VirtIODevice *vdev, VirtQueue *vq)
{
    (void)vdev;
    (void)vq;
}

static uint64_t tebox_v4l2_get_features(VirtIODevice *vdev, uint64_t features,
                                        Error **errp)
{
    (void)vdev; (void)errp;
    return features;
}

static void tebox_v4l2_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    TeboxV4l2 *v = TEBOX_V4L2(dev);
    virtio_init(vdev, TEBOX_V4L2_VIRTIO_ID, 0);
    v->cmd_vq = virtio_add_queue(vdev, 64, tebox_v4l2_handle_cmd);
    v->evt_vq = virtio_add_queue(vdev, 64, tebox_v4l2_handle_evt);
    for (int i = 0; i < TEBOX_V4L2_MAX_STREAMS; i++) {
        v->streams[i].fd = -1;
    }
    info_report("tebox-v4l2: virtio device ready (id=%d)", TEBOX_V4L2_VIRTIO_ID);
}

static void tebox_v4l2_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    TeboxV4l2 *v = TEBOX_V4L2(dev);
    for (int i = 0; i < TEBOX_V4L2_MAX_STREAMS; i++) {
        if (v->streams[i].used && v->streams[i].fd >= 0) {
            tbx_stop(&v->streams[i]);
            close(v->streams[i].fd);
        }
    }
    virtio_delete_queue(v->cmd_vq);
    virtio_delete_queue(v->evt_vq);
    virtio_cleanup(vdev);
}

static const VMStateDescription tebox_v4l2_vmstate = {
    .name = "tebox-v4l2",
    .unmigratable = 1,
};

static void tebox_v4l2_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    (void)data;
    dc->vmsd = &tebox_v4l2_vmstate;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = tebox_v4l2_realize;
    vdc->unrealize = tebox_v4l2_unrealize;
    vdc->get_features = tebox_v4l2_get_features;
}

static const TypeInfo tebox_v4l2_info = {
    .name = TYPE_TEBOX_V4L2,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(TeboxV4l2),
    .class_init = tebox_v4l2_class_init,
};

static void tebox_v4l2_register_types(void)
{
    type_register_static(&tebox_v4l2_info);
}

type_init(tebox_v4l2_register_types)

/* ---- virtio-pci transport wrapper: -device tebox-v4l2-pci ---------------- */

#define TYPE_TEBOX_V4L2_PCI "tebox-v4l2-pci-base"
typedef struct TeboxV4l2PCI TeboxV4l2PCI;
DECLARE_INSTANCE_CHECKER(TeboxV4l2PCI, TEBOX_V4L2_PCI, TYPE_TEBOX_V4L2_PCI)

struct TeboxV4l2PCI {
    VirtIOPCIProxy parent_obj;
    TeboxV4l2 vdev;
};

static void tebox_v4l2_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    TeboxV4l2PCI *dev = TEBOX_V4L2_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);
    qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void tebox_v4l2_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev_k = PCI_DEVICE_CLASS(klass);
    (void)data;
    k->realize = tebox_v4l2_pci_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    pcidev_k->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pcidev_k->device_id = 0x1040 + TEBOX_V4L2_VIRTIO_ID;  /* modern virtio-pci */
    pcidev_k->revision = VIRTIO_PCI_ABI_VERSION;
    pcidev_k->class_id = PCI_CLASS_OTHERS;
}

static void tebox_v4l2_pci_initfn(Object *obj)
{
    TeboxV4l2PCI *dev = TEBOX_V4L2_PCI(obj);
    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_TEBOX_V4L2);
}

static const VirtioPCIDeviceTypeInfo tebox_v4l2_pci_info = {
    .base_name = TYPE_TEBOX_V4L2_PCI,
    .generic_name = "tebox-v4l2-pci",
    .instance_size = sizeof(TeboxV4l2PCI),
    .instance_init = tebox_v4l2_pci_initfn,
    .class_init = tebox_v4l2_pci_class_init,
};

static void tebox_v4l2_pci_register(void)
{
    virtio_pci_types_register(&tebox_v4l2_pci_info);
}

type_init(tebox_v4l2_pci_register)
