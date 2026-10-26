/* Direct V4L2 M2M video encoder (Linux), used ahead of FFmpeg.
 *
 * FFmpeg's h264_v4l2m2m crashes in avcodec_send_frame on the CIX Sky1
 * "mvx" encoder, so frames go to the device here: RGBA/BGRA input as
 * AB24/AR24 (the VPU converts colour), one frame in flight. H.264/HEVC
 * cache SPS(/VPS)/PPS and re-send with every IDR; VP8/VP9 return raw
 * frames. Mirrors virglrenderer's vrend_video_v4l2.c.
 */
#include "v4l2enc.h"

#ifdef __linux__

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/videodev2.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#define NUM_OUTPUT 2
#define NUM_CAPTURE 4
#define ENCODE_TIMEOUT_MS 500

#ifndef V4L2_PIX_FMT_HEVC
#define V4L2_PIX_FMT_HEVC v4l2_fourcc('H', 'E', 'V', 'C')
#endif
#ifndef V4L2_PIX_FMT_VP8
#define V4L2_PIX_FMT_VP8 v4l2_fourcc('V', 'P', '8', '0')
#endif
#ifndef V4L2_PIX_FMT_VP9
#define V4L2_PIX_FMT_VP9 v4l2_fourcc('V', 'P', '9', '0')
#endif

struct plane_map {
    void *ptr;
    size_t length;
};

struct buf_map {
    struct plane_map planes[VIDEO_MAX_PLANES];
    unsigned num_planes;
};

struct cast_v4l2 {
    int fd;
    char path[64];
    int width, height;
    enum cast_codec codec;
    uint32_t out_fourcc;
    uint32_t fourcc; /* input RGBA/BGRA fourcc */
    struct v4l2_format in_fmt;
    uint32_t out_memory; /* MMAP, or DMABUF for frames already in a dma-buf */
    struct buf_map output[NUM_OUTPUT], capture[NUM_CAPTURE];
    unsigned num_output, num_capture, next_output;
    uint8_t *headers;
    size_t headers_size;
    uint8_t *encoded;
    size_t encoded_size, encoded_cap;
    unsigned frames;
};

static uint32_t codec_pixelformat(enum cast_codec codec)
{
    switch (codec) {
    case CAST_CODEC_HEVC:
        return V4L2_PIX_FMT_HEVC;
    case CAST_CODEC_VP8:
        return V4L2_PIX_FMT_VP8;
    case CAST_CODEC_VP9:
        return V4L2_PIX_FMT_VP9;
    case CAST_CODEC_H264:
    default:
        return V4L2_PIX_FMT_H264;
    }
}

static bool codec_is_annexb(enum cast_codec codec)
{
    return codec == CAST_CODEC_H264 || codec == CAST_CODEC_HEVC;
}

static int xioctl(int fd, unsigned long request, void *arg)
{
    int r;

    do {
        r = ioctl(fd, request, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

static bool encodes_pixelformat(const char *path, uint32_t pixelformat)
{
    struct v4l2_capability cap;
    int fd = open(path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    bool ok = false;

    if (fd < 0) {
        return false;
    }
    memset(&cap, 0, sizeof(cap));
    if (!xioctl(fd, VIDIOC_QUERYCAP, &cap)) {
        uint32_t caps = cap.capabilities & V4L2_CAP_DEVICE_CAPS ? cap.device_caps
                                                                : cap.capabilities;

        for (unsigned i = 0; !ok && (caps & V4L2_CAP_VIDEO_M2M_MPLANE); i++) {
            struct v4l2_fmtdesc desc = {
                .index = i, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
            };

            if (xioctl(fd, VIDIOC_ENUM_FMT, &desc)) {
                break;
            }
            ok = desc.pixelformat == pixelformat;
        }
    }
    close(fd);
    return ok;
}

static bool find_device(char *path, size_t size, uint32_t pixelformat)
{
    const char *forced = getenv("TEBOX_CAST_V4L2_DEVICE");
    DIR *dir;
    struct dirent *e;
    bool found = false;

    if (forced && forced[0]) {
        snprintf(path, size, "%s", forced);
        return encodes_pixelformat(path, pixelformat);
    }
    dir = opendir("/dev");
    if (!dir) {
        return false;
    }
    while (!found && (e = readdir(dir))) {
        if (!strncmp(e->d_name, "video", 5) && strlen(e->d_name) < size - 6) {
            snprintf(path, size, "/dev/%s", e->d_name);
            found = encodes_pixelformat(path, pixelformat);
        }
    }
    closedir(dir);
    return found;
}

static void set_ctrl(struct cast_v4l2 *e, uint32_t id, int32_t value)
{
    struct v4l2_control ctrl = { .id = id, .value = value };

    if (xioctl(e->fd, VIDIOC_S_CTRL, &ctrl)) {
        fprintf(stderr, "cast-v4l2: control %#x=%d: %s\n", id, value,
                strerror(errno));
    }
}

static bool map_buffers(struct cast_v4l2 *e, uint32_t type, struct buf_map *maps,
                        unsigned *count, unsigned wanted)
{
    struct v4l2_requestbuffers req = {
        .count = wanted, .type = type, .memory = V4L2_MEMORY_MMAP,
    };

    if (xioctl(e->fd, VIDIOC_REQBUFS, &req) || !req.count) {
        return false;
    }
    *count = req.count < wanted ? req.count : wanted;
    for (unsigned i = 0; i < *count; i++) {
        struct v4l2_plane planes[VIDEO_MAX_PLANES];
        struct v4l2_buffer buf = {
            .index = i, .type = type, .memory = V4L2_MEMORY_MMAP,
            .m.planes = planes, .length = VIDEO_MAX_PLANES,
        };

        memset(planes, 0, sizeof(planes));
        if (xioctl(e->fd, VIDIOC_QUERYBUF, &buf)) {
            return false;
        }
        maps[i].num_planes = buf.length;
        for (unsigned p = 0; p < buf.length; p++) {
            maps[i].planes[p].length = planes[p].length;
            maps[i].planes[p].ptr = mmap(NULL, planes[p].length,
                                         PROT_READ | PROT_WRITE, MAP_SHARED,
                                         e->fd, planes[p].m.mem_offset);
            if (maps[i].planes[p].ptr == MAP_FAILED) {
                maps[i].planes[p].ptr = NULL;
                return false;
            }
        }
    }
    return true;
}

static bool queue_capture(struct cast_v4l2 *e, unsigned index)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct v4l2_buffer buf = {
        .index = index, .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE,
        .memory = V4L2_MEMORY_MMAP, .m.planes = planes,
        .length = e->capture[index].num_planes,
    };

    memset(planes, 0, sizeof(planes));
    for (unsigned p = 0; p < buf.length; p++) {
        planes[p].length = e->capture[index].planes[p].length;
    }
    return !xioctl(e->fd, VIDIOC_QBUF, &buf);
}

static bool append(uint8_t **data, size_t *size, size_t *cap,
                   const uint8_t *src, size_t len)
{
    if (*size + len > *cap) {
        size_t ncap = (*size + len) * 2;
        uint8_t *n = realloc(*data, ncap);

        if (!n) {
            return false;
        }
        *data = n;
        *cap = ncap;
    }
    memcpy(*data + *size, src, len);
    *size += len;
    return true;
}

/* Bitmask of the H.264 NAL unit types in an Annex-B buffer. */
static uint32_t h264_nal_types(const uint8_t *d, size_t n)
{
    uint32_t mask = 0;

    for (size_t i = 0; i + 3 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            mask |= 1u << (d[i + 3] & 0x1f);
            i += 2;
        }
    }
    return mask;
}

#define H264_SLICE_MASK ((1u << 1) | (1u << 5))
#define H264_IDR_MASK   (1u << 5)
#define H264_SPS_MASK   (1u << 7)

static void hevc_scan(const uint8_t *d, size_t n, bool *has_slice, bool *has_idr,
                      bool *has_param)
{
    *has_slice = *has_idr = *has_param = false;
    for (size_t i = 0; i + 4 < n; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            uint8_t t = (d[i + 3] >> 1) & 0x3f;

            if (t <= 9 || (t >= 16 && t <= 21)) {
                *has_slice = true;
            }
            if (t == 19 || t == 20) {
                *has_idr = true;
            }
            if (t == 32 || t == 33 || t == 34) {
                *has_param = true;
            }
            i += 2;
        }
    }
}

void cast_v4l2_close(struct cast_v4l2 *e)
{
    int type;

    if (!e) {
        return;
    }
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    xioctl(e->fd, VIDIOC_STREAMOFF, &type);
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    xioctl(e->fd, VIDIOC_STREAMOFF, &type);
    for (unsigned i = 0; i < NUM_OUTPUT; i++) {
        for (unsigned p = 0; p < e->output[i].num_planes; p++) {
            if (e->output[i].planes[p].ptr) {
                munmap(e->output[i].planes[p].ptr, e->output[i].planes[p].length);
            }
        }
    }
    for (unsigned i = 0; i < NUM_CAPTURE; i++) {
        for (unsigned p = 0; p < e->capture[i].num_planes; p++) {
            if (e->capture[i].planes[p].ptr) {
                munmap(e->capture[i].planes[p].ptr, e->capture[i].planes[p].length);
            }
        }
    }
    close(e->fd);
    free(e->headers);
    free(e->encoded);
    free(e);
}

struct cast_v4l2 *cast_v4l2_open(enum cast_codec codec, int width, int height,
                                 bool rgba, int fps, int64_t bitrate,
                                 int gop_frames, bool dmabuf_input, int stride)
{
    struct cast_v4l2 *e = calloc(1, sizeof(*e));
    struct v4l2_format fmt;
    struct v4l2_streamparm parm;
    int type;

    if (!e) {
        return NULL;
    }
    if (codec != CAST_CODEC_H264 && codec != CAST_CODEC_HEVC &&
        codec != CAST_CODEC_VP8 && codec != CAST_CODEC_VP9) {
        codec = CAST_CODEC_H264;
    }
    e->fd = -1;
    e->codec = codec;
    e->out_fourcc = codec_pixelformat(codec);
    if (!find_device(e->path, sizeof(e->path), e->out_fourcc)) {
        free(e);
        return NULL;
    }
    e->fd = open(e->path, O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (e->fd < 0) {
        free(e);
        return NULL;
    }
    e->width = width;
    e->height = height;
    /* AB24 = R,G,B,A bytes; AR24 = B,G,R,A bytes. */
    e->fourcc = rgba ? v4l2_fourcc('A', 'B', '2', '4') : v4l2_fourcc('A', 'R', '2', '4');

    memset(&fmt, 0, sizeof(fmt));
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.pixelformat = e->out_fourcc;
    fmt.fmt.pix_mp.width = width;
    fmt.fmt.pix_mp.height = height;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = width * height * 3 / 2 + 4096;
    if (xioctl(e->fd, VIDIOC_S_FMT, &fmt)) {
        goto fail;
    }
    e->in_fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    e->in_fmt.fmt.pix_mp.pixelformat = e->fourcc;
    e->in_fmt.fmt.pix_mp.width = width;
    e->in_fmt.fmt.pix_mp.height = height;
    e->in_fmt.fmt.pix_mp.num_planes = 1;
    /* dma-buf frames keep their producer's pitch (gfxstream aligns rows to
     * 64 bytes); the mvx encoder honours a requested bytesperline. */
    if (dmabuf_input && stride > 0) {
        e->in_fmt.fmt.pix_mp.plane_fmt[0].bytesperline = (uint32_t)stride;
    }
    if (xioctl(e->fd, VIDIOC_S_FMT, &e->in_fmt) ||
        e->in_fmt.fmt.pix_mp.pixelformat != e->fourcc) {
        goto fail;
    }
    memset(&parm, 0, sizeof(parm));
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = fps > 0 ? fps : 60;
    xioctl(e->fd, VIDIOC_S_PARM, &parm);

    if (codec == CAST_CODEC_H264) {
        set_ctrl(e, V4L2_CID_MPEG_VIDEO_H264_PROFILE,
                 V4L2_MPEG_VIDEO_H264_PROFILE_MAIN);
#ifdef V4L2_CID_MPEG_VIDEO_HEVC_PROFILE
    } else if (codec == CAST_CODEC_HEVC) {
        set_ctrl(e, V4L2_CID_MPEG_VIDEO_HEVC_PROFILE,
                 V4L2_MPEG_VIDEO_HEVC_PROFILE_MAIN);
#endif
    }
    set_ctrl(e, V4L2_CID_MPEG_VIDEO_B_FRAMES, 0);
    set_ctrl(e, V4L2_CID_MPEG_VIDEO_GOP_SIZE, gop_frames > 0 ? gop_frames : 60);
    set_ctrl(e, V4L2_CID_MPEG_VIDEO_BITRATE_MODE, V4L2_MPEG_VIDEO_BITRATE_MODE_CBR);
    set_ctrl(e, V4L2_CID_MPEG_VIDEO_BITRATE, (int32_t)bitrate);

    e->out_memory = dmabuf_input ? V4L2_MEMORY_DMABUF : V4L2_MEMORY_MMAP;
    if (dmabuf_input) {
        struct v4l2_requestbuffers req = {
            .count = NUM_OUTPUT, .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE,
            .memory = V4L2_MEMORY_DMABUF,
        };

        if (xioctl(e->fd, VIDIOC_REQBUFS, &req) || !req.count) {
            goto fail;
        }
        e->num_output = req.count < NUM_OUTPUT ? req.count : NUM_OUTPUT;
    } else if (!map_buffers(e, V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, e->output,
                            &e->num_output, NUM_OUTPUT)) {
        goto fail;
    }
    if (!map_buffers(e, V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, e->capture,
                     &e->num_capture, NUM_CAPTURE)) {
        goto fail;
    }
    for (unsigned i = 0; i < e->num_capture; i++) {
        if (!queue_capture(e, i)) {
            goto fail;
        }
    }
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (xioctl(e->fd, VIDIOC_STREAMON, &type)) {
        goto fail;
    }
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (xioctl(e->fd, VIDIOC_STREAMON, &type)) {
        goto fail;
    }
    fprintf(stderr, "cast-v4l2: %s %s %dx%d input=%.4s out=%.4s stride=%u%s\n",
            e->path, cast_codec_name(codec), width, height,
            (const char *)&e->fourcc, (const char *)&e->out_fourcc,
            e->in_fmt.fmt.pix_mp.plane_fmt[0].bytesperline,
            dmabuf_input ? " dma-buf (zero-copy)" : "");
    return e;

fail:
    fprintf(stderr, "cast-v4l2: %s setup failed: %s\n", e->path, strerror(errno));
    cast_v4l2_close(e);
    return NULL;
}

static void reclaim_outputs(struct cast_v4l2 *e)
{
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct v4l2_buffer buf = {
        .type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE, .memory = e->out_memory,
        .m.planes = planes, .length = VIDEO_MAX_PLANES,
    };

    while (!xioctl(e->fd, VIDIOC_DQBUF, &buf)) {
    }
}

static int collect(struct cast_v4l2 *e, const uint8_t **out, size_t *out_size, bool *key);

int cast_v4l2_encode(struct cast_v4l2 *e, const uint8_t *pixels, int stride,
                     bool force_idr, const uint8_t **out, size_t *out_size,
                     bool *key)
{
    const struct v4l2_pix_format_mplane *pix = &e->in_fmt.fmt.pix_mp;
    unsigned index = e->next_output;
    struct buf_map *map = &e->output[index];
    size_t dst_stride = pix->plane_fmt[0].bytesperline;
    size_t row = (size_t)e->width * 4;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct v4l2_buffer buf;

    *out = NULL;
    *out_size = 0;
    *key = false;
    if (e->out_memory != V4L2_MEMORY_MMAP) {
        return -1;
    }
    reclaim_outputs(e);
    for (int y = 0; y < e->height; y++) {
        memcpy((uint8_t *)map->planes[0].ptr + (size_t)y * dst_stride,
               pixels + (size_t)y * stride, row);
    }
    if (force_idr) {
        set_ctrl(e, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1);
    }
    memset(planes, 0, sizeof(planes));
    memset(&buf, 0, sizeof(buf));
    buf.index = index;
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.m.planes = planes;
    buf.length = map->num_planes;
    for (unsigned p = 0; p < map->num_planes; p++) {
        planes[p].length = map->planes[p].length;
        planes[p].bytesused = p == 0 ? pix->plane_fmt[0].sizeimage : 0;
    }
    if (xioctl(e->fd, VIDIOC_QBUF, &buf)) {
        fprintf(stderr, "cast-v4l2: QBUF input: %s\n", strerror(errno));
        return -1;
    }
    e->next_output = (index + 1) % e->num_output;
    return collect(e, out, out_size, key);
}

int cast_v4l2_encode_dmabuf(struct cast_v4l2 *e, int fd, size_t length, bool force_idr,
                            const uint8_t **out, size_t *out_size, bool *key)
{
    const struct v4l2_pix_format_mplane *pix = &e->in_fmt.fmt.pix_mp;
    struct v4l2_plane planes[VIDEO_MAX_PLANES];
    struct v4l2_buffer buf;

    *out = NULL;
    *out_size = 0;
    *key = false;
    if (e->out_memory != V4L2_MEMORY_DMABUF || length < pix->plane_fmt[0].sizeimage) {
        return -1;
    }
    reclaim_outputs(e);
    if (force_idr) {
        set_ctrl(e, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1);
    }
    memset(planes, 0, sizeof(planes));
    memset(&buf, 0, sizeof(buf));
    buf.index = e->next_output;
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.m.planes = planes;
    buf.length = 1;
    planes[0].m.fd = fd;
    planes[0].length = (uint32_t)length;
    planes[0].bytesused = pix->plane_fmt[0].sizeimage;
    if (xioctl(e->fd, VIDIOC_QBUF, &buf)) {
        fprintf(stderr, "cast-v4l2: QBUF dma-buf input: %s\n", strerror(errno));
        return -1;
    }
    e->next_output = (e->next_output + 1) % e->num_output;
    return collect(e, out, out_size, key);
}

int cast_v4l2_input_stride(const struct cast_v4l2 *e)
{
    return e ? (int)e->in_fmt.fmt.pix_mp.plane_fmt[0].bytesperline : 0;
}

bool cast_v4l2_dmabuf_input(const struct cast_v4l2 *e)
{
    return e && e->out_memory == V4L2_MEMORY_DMABUF;
}

/* Waits for the encoded slice/frame of the buffer just queued. */
static int collect(struct cast_v4l2 *e, const uint8_t **out, size_t *out_size, bool *key)
{
    uint8_t *frame = NULL;
    size_t frame_size = 0, frame_cap = 0;
    bool have_slice = false;
    bool key_flag = false;

    /* Parameter sets may arrive in a buffer of their own before the slice. */
    while (!have_slice) {
        struct pollfd pfd = { .fd = e->fd, .events = POLLIN };
        struct v4l2_plane cplanes[VIDEO_MAX_PLANES];
        struct v4l2_buffer cbuf = {
            .type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE, .memory = V4L2_MEMORY_MMAP,
            .m.planes = cplanes, .length = VIDEO_MAX_PLANES,
        };

        if (poll(&pfd, 1, ENCODE_TIMEOUT_MS) <= 0 || (pfd.revents & POLLERR)) {
            fprintf(stderr, "cast-v4l2: no encoded output\n");
            break;
        }
        memset(cplanes, 0, sizeof(cplanes));
        if (xioctl(e->fd, VIDIOC_DQBUF, &cbuf)) {
            if (errno == EAGAIN) {
                continue;
            }
            fprintf(stderr, "cast-v4l2: DQBUF: %s\n", strerror(errno));
            break;
        }
        if (cbuf.index < e->num_capture) {
            const uint8_t *data = (const uint8_t *)e->capture[cbuf.index].planes[0].ptr +
                                  cplanes[0].data_offset;
            size_t len = cplanes[0].bytesused > cplanes[0].data_offset
                             ? cplanes[0].bytesused - cplanes[0].data_offset : 0;

            if (cbuf.flags & V4L2_BUF_FLAG_KEYFRAME) {
                key_flag = true;
            }
            if (!codec_is_annexb(e->codec)) {
                if (len) {
                    append(&frame, &frame_size, &frame_cap, data, len);
                    have_slice = true;
                }
            } else if (e->codec == CAST_CODEC_HEVC) {
                bool slice = false, idr = false, param = false;

                hevc_scan(data, len, &slice, &idr, &param);
                if (slice) {
                    append(&frame, &frame_size, &frame_cap, data, len);
                    have_slice = true;
                    if (idr) {
                        key_flag = true;
                    }
                } else if (len && param) {
                    size_t hcap = 0;

                    free(e->headers);
                    e->headers = NULL;
                    e->headers_size = 0;
                    append(&e->headers, &e->headers_size, &hcap, data, len);
                }
            } else {
                uint32_t types = h264_nal_types(data, len);

                if (types & H264_SLICE_MASK) {
                    append(&frame, &frame_size, &frame_cap, data, len);
                    have_slice = true;
                } else if (len && (types & H264_SPS_MASK)) {
                    size_t hcap = 0;

                    free(e->headers);
                    e->headers = NULL;
                    e->headers_size = 0;
                    append(&e->headers, &e->headers_size, &hcap, data, len);
                }
            }
            queue_capture(e, cbuf.index);
        }
    }
    if (!have_slice) {
        free(frame);
        return -1;
    }

    e->encoded_size = 0;
    *key = key_flag;
    if (e->codec == CAST_CODEC_H264) {
        uint32_t types = h264_nal_types(frame, frame_size);

        *key = (types & H264_IDR_MASK) != 0;
        if (types & H264_SPS_MASK) {
            size_t end = 0, hcap = 0;

            for (size_t i = 0; i + 3 < frame_size; i++) {
                if (frame[i] == 0 && frame[i + 1] == 0 && frame[i + 2] == 1 &&
                    ((1u << (frame[i + 3] & 0x1f)) & H264_SLICE_MASK)) {
                    end = i > 0 && frame[i - 1] == 0 ? i - 1 : i;
                    break;
                }
            }
            if (end) {
                free(e->headers);
                e->headers = NULL;
                e->headers_size = 0;
                append(&e->headers, &e->headers_size, &hcap, frame, end);
            }
        } else if (*key && e->headers_size) {
            append(&e->encoded, &e->encoded_size, &e->encoded_cap, e->headers,
                   e->headers_size);
        }
    } else if (e->codec == CAST_CODEC_HEVC) {
        bool slice = false, idr = false, param = false;

        hevc_scan(frame, frame_size, &slice, &idr, &param);
        *key = idr || key_flag;
        if (param) {
            size_t end = 0, hcap = 0;

            for (size_t i = 0; i + 4 < frame_size; i++) {
                if (frame[i] == 0 && frame[i + 1] == 0 && frame[i + 2] == 1) {
                    uint8_t t = (frame[i + 3] >> 1) & 0x3f;

                    if (t <= 9 || (t >= 16 && t <= 21)) {
                        end = i > 0 && frame[i - 1] == 0 ? i - 1 : i;
                        break;
                    }
                }
            }
            if (end) {
                free(e->headers);
                e->headers = NULL;
                e->headers_size = 0;
                append(&e->headers, &e->headers_size, &hcap, frame, end);
            }
        } else if (*key && e->headers_size) {
            append(&e->encoded, &e->encoded_size, &e->encoded_cap, e->headers,
                   e->headers_size);
        }
    } else if (e->codec == CAST_CODEC_VP8 && frame_size > 0) {
        /* VP8: keyframe when frame tag bit 0 is clear. */
        *key = key_flag || ((frame[0] & 1) == 0);
    } else if (e->codec == CAST_CODEC_VP9 && frame_size > 0) {
        /* VP9: uncompressed header frame_type bit after marker. */
        *key = key_flag || ((frame[0] & 0x4) == 0);
    }
    append(&e->encoded, &e->encoded_size, &e->encoded_cap, frame, frame_size);
    free(frame);
    if (e->frames++ < 3) {
        fprintf(stderr, "cast-v4l2: frame %u %s bytes=%zu key=%d headers=%zu\n",
                e->frames - 1, cast_codec_name(e->codec), e->encoded_size,
                *key ? 1 : 0, e->headers_size);
    }
    *out = e->encoded;
    *out_size = e->encoded_size;
    return 0;
}

const char *cast_v4l2_device(const struct cast_v4l2 *e)
{
    return e ? e->path : "";
}

enum cast_codec cast_v4l2_codec(const struct cast_v4l2 *e)
{
    return e ? e->codec : CAST_CODEC_H264;
}

#else /* !__linux__ */

struct cast_v4l2 *cast_v4l2_open(enum cast_codec codec, int width, int height,
                                 bool rgba, int fps, int64_t bitrate,
                                 int gop_frames, bool dmabuf_input, int stride)
{
    (void)codec; (void)width; (void)height; (void)rgba; (void)fps; (void)bitrate;
    (void)gop_frames; (void)dmabuf_input; (void)stride;
    return NULL;
}

int cast_v4l2_encode_dmabuf(struct cast_v4l2 *e, int fd, size_t length, bool force_idr,
                            const uint8_t **out, size_t *out_size, bool *key)
{
    (void)e; (void)fd; (void)length; (void)force_idr; (void)out; (void)out_size;
    (void)key;
    return -1;
}

int cast_v4l2_input_stride(const struct cast_v4l2 *e)
{
    (void)e;
    return 0;
}

bool cast_v4l2_dmabuf_input(const struct cast_v4l2 *e)
{
    (void)e;
    return false;
}

int cast_v4l2_encode(struct cast_v4l2 *e, const uint8_t *pixels, int stride,
                     bool force_idr, const uint8_t **out, size_t *out_size,
                     bool *key)
{
    (void)e; (void)pixels; (void)stride; (void)force_idr; (void)out;
    (void)out_size; (void)key;
    return -1;
}

void cast_v4l2_close(struct cast_v4l2 *e)
{
    (void)e;
}

const char *cast_v4l2_device(const struct cast_v4l2 *e)
{
    (void)e;
    return "";
}

enum cast_codec cast_v4l2_codec(const struct cast_v4l2 *e)
{
    (void)e;
    return CAST_CODEC_H264;
}

#endif
