/* Guest media bridge: virtio-serial sockets <-> viewers.
 *
 * QEMU serves three chardev sockets next to cast.sock (boot-qemu.sh):
 * guest PCM from audio-out.sock is AAC-encoded for the viewers, viewer
 * microphone PCM goes to audio-in.sock, and viewer camera JPEGs are decoded,
 * scaled to CAST_CAMERA_WIDTH x CAST_CAMERA_HEIGHT and written to
 * camera.sock as NV12 frames for the camera HAL.
 */
#include "cast/media.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>

#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <time.h>
#include <unistd.h>

#define RECONNECT_MS 500
#define AAC_BITRATE 128000
/* Viewer mic audio beyond this is dropped instead of queued (~200 ms). */
#define MIC_BACKLOG (CAST_AUDIO_RATE * 2 / 5)

struct chan {
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    const char *name;
    int fd;
    uint64_t retry_ms;
    uint8_t *pending;      /* bytes of a record not yet written */
    size_t pending_len, pending_cap;
};

struct cast_media {
    struct chan out, in, cam;

    AVCodecContext *aac;
    AVFrame *aac_frame;
    AVPacket *aac_pkt;
    int16_t *pcm;          /* interleaved stereo s16 waiting for a full frame */
    size_t pcm_frames, pcm_cap;
    uint64_t samples_out;

    AVCodecContext *jpeg;
    AVFrame *jpeg_frame;
    AVPacket *jpeg_pkt;
    struct SwsContext *sws;
    uint8_t *nv12;
};

static uint64_t now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000 + (uint64_t)ts.tv_nsec / 1000000;
}

static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000 + (uint64_t)ts.tv_nsec / 1000;
}

static void chan_init(struct chan *c, const char *dir, const char *file,
                      const char *name)
{
    memset(c, 0, sizeof(*c));
    c->fd = -1;
    c->name = name;
    snprintf(c->path, sizeof(c->path), "%s/%s", dir, file);
}

static void chan_drop(struct chan *c)
{
    if (c->fd >= 0) {
        close(c->fd);
        fprintf(stderr, "cast-media: %s disconnected\n", c->name);
    }
    c->fd = -1;
    c->pending_len = 0;
    c->retry_ms = now_ms() + RECONNECT_MS;
}

static bool chan_ready(struct chan *c)
{
    struct sockaddr_un addr;
    int fd;

    if (c->fd >= 0) {
        return true;
    }
    if (now_ms() < c->retry_ms) {
        return false;
    }
    c->retry_ms = now_ms() + RECONNECT_MS;
    fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) {
        return false;
    }
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    memcpy(addr.sun_path, c->path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(fd);
        return false;
    }
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#ifdef SO_NOSIGPIPE
    {
        int one = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
    }
#endif
    c->fd = fd;
    fprintf(stderr, "cast-media: %s connected (%s)\n", c->name, c->path);
    return true;
}

/* Finish a partially written record. Returns true once nothing is pending. */
static bool chan_flush(struct chan *c)
{
    size_t off = 0;

    while (off < c->pending_len) {
        ssize_t n = send(c->fd, c->pending + off, c->pending_len - off,
#ifdef MSG_NOSIGNAL
                         MSG_NOSIGNAL |
#endif
                         MSG_DONTWAIT);

        if (n > 0) {
            off += (size_t)n;
        } else if (n < 0 && errno == EINTR) {
            continue;
        } else if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
            break;
        } else {
            chan_drop(c);
            return false;
        }
    }
    memmove(c->pending, c->pending + off, c->pending_len - off);
    c->pending_len -= off;
    return c->pending_len == 0;
}

/* Queue a whole record (parts concatenated) unless one is still pending: a
 * busy guest loses this record instead of seeing a torn one. */
static void chan_write(struct chan *c, const void *a, size_t alen,
                       const void *b, size_t blen)
{
    if (!chan_ready(c) || !chan_flush(c)) {
        return;
    }
    if (alen + blen > c->pending_cap) {
        uint8_t *n = realloc(c->pending, alen + blen);

        if (!n) {
            return;
        }
        c->pending = n;
        c->pending_cap = alen + blen;
    }
    memcpy(c->pending, a, alen);
    if (blen) {
        memcpy(c->pending + alen, b, blen);
    }
    c->pending_len = alen + blen;
    chan_flush(c);
}

static void adts_header(uint8_t h[7], size_t payload)
{
    const size_t len = payload + 7;
    const int profile = 1;   /* AAC-LC (object type 2) - 1 */
    const int rate_index = 3; /* 48000 Hz */
    const int channels = 2;

    h[0] = 0xff;
    h[1] = 0xf1; /* MPEG-4, no CRC */
    h[2] = (uint8_t)((profile << 6) | (rate_index << 2) | (channels >> 2));
    h[3] = (uint8_t)(((channels & 3) << 6) | ((len >> 11) & 3));
    h[4] = (uint8_t)((len >> 3) & 0xff);
    h[5] = (uint8_t)(((len & 7) << 5) | 0x1f);
    h[6] = 0xfc;
}

static bool aac_open(struct cast_media *m)
{
    const AVCodec *codec = avcodec_find_encoder_by_name("aac");
    AVChannelLayout stereo = AV_CHANNEL_LAYOUT_STEREO;

    if (!codec) {
        fprintf(stderr, "cast-media: no aac encoder, guest audio disabled\n");
        return false;
    }
    m->aac = avcodec_alloc_context3(codec);
    m->aac_frame = av_frame_alloc();
    m->aac_pkt = av_packet_alloc();
    if (!m->aac || !m->aac_frame || !m->aac_pkt) {
        return false;
    }
    m->aac->sample_rate = CAST_AUDIO_RATE;
    m->aac->sample_fmt = AV_SAMPLE_FMT_FLTP;
    m->aac->bit_rate = AAC_BITRATE;
    m->aac->time_base = (AVRational){1, CAST_AUDIO_RATE};
    av_channel_layout_copy(&m->aac->ch_layout, &stereo);
    if (avcodec_open2(m->aac, codec, NULL) < 0) {
        fprintf(stderr, "cast-media: aac encoder open failed\n");
        avcodec_free_context(&m->aac);
        return false;
    }
    m->aac_frame->nb_samples = m->aac->frame_size;
    m->aac_frame->format = AV_SAMPLE_FMT_FLTP;
    m->aac_frame->sample_rate = CAST_AUDIO_RATE;
    av_channel_layout_copy(&m->aac_frame->ch_layout, &stereo);
    if (av_frame_get_buffer(m->aac_frame, 0) < 0) {
        avcodec_free_context(&m->aac);
        return false;
    }
    return true;
}

static void aac_encode(struct cast_media *m, cast_media_audio_cb cb, void *user)
{
    const int fs = m->aac->frame_size;

    while (m->pcm_frames >= (size_t)fs) {
        float *l, *r;

        if (av_frame_make_writable(m->aac_frame) < 0) {
            return;
        }
        l = (float *)m->aac_frame->data[0];
        r = (float *)m->aac_frame->data[1];
        for (int i = 0; i < fs; i++) {
            l[i] = m->pcm[2 * i] / 32768.0f;
            r[i] = m->pcm[2 * i + 1] / 32768.0f;
        }
        memmove(m->pcm, m->pcm + 2 * fs, (m->pcm_frames - fs) * 4);
        m->pcm_frames -= fs;
        m->aac_frame->pts = (int64_t)m->samples_out;
        m->samples_out += fs;
        if (avcodec_send_frame(m->aac, m->aac_frame) < 0) {
            continue;
        }
        while (avcodec_receive_packet(m->aac, m->aac_pkt) == 0) {
            uint8_t *rec = malloc(m->aac_pkt->size + 7);

            if (rec) {
                adts_header(rec, (size_t)m->aac_pkt->size);
                memcpy(rec + 7, m->aac_pkt->data, m->aac_pkt->size);
                cb(user, rec, (size_t)m->aac_pkt->size + 7,
                   (uint64_t)m->aac_pkt->pts * 1000000 / CAST_AUDIO_RATE);
                free(rec);
            }
            av_packet_unref(m->aac_pkt);
        }
    }
}

struct cast_media *cast_media_open(const char *dir)
{
    struct cast_media *m = calloc(1, sizeof(*m));

    if (!m) {
        return NULL;
    }
    chan_init(&m->out, dir, "audio-out.sock", "audio-out");
    chan_init(&m->in, dir, "audio-in.sock", "audio-in");
    chan_init(&m->cam, dir, "camera.sock", "camera");
    aac_open(m);
    return m;
}

void cast_media_close(struct cast_media *m)
{
    if (!m) {
        return;
    }
    chan_drop(&m->out);
    chan_drop(&m->in);
    chan_drop(&m->cam);
    free(m->out.pending);
    free(m->in.pending);
    free(m->cam.pending);
    avcodec_free_context(&m->aac);
    av_frame_free(&m->aac_frame);
    av_packet_free(&m->aac_pkt);
    avcodec_free_context(&m->jpeg);
    av_frame_free(&m->jpeg_frame);
    av_packet_free(&m->jpeg_pkt);
    sws_freeContext(m->sws);
    free(m->pcm);
    free(m->nv12);
    free(m);
}

void cast_media_poll(struct cast_media *m, int want_audio,
                     cast_media_audio_cb cb, void *user)
{
    uint8_t buf[16384];

    if (!m) {
        return;
    }
    /* The write-side sockets only need their pending bytes pushed. */
    if (m->in.fd >= 0) {
        chan_flush(&m->in);
    }
    if (m->cam.fd >= 0) {
        chan_flush(&m->cam);
    }
    if (!chan_ready(&m->out)) {
        return;
    }
    for (;;) {
        ssize_t n = recv(m->out.fd, buf, sizeof(buf), MSG_DONTWAIT);

        if (n == 0 || (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                       errno != EINTR)) {
            chan_drop(&m->out);
            return;
        }
        if (n < 0) {
            break;
        }
        if (!want_audio || !m->aac) {
            m->pcm_frames = 0;
            continue;
        }
        /* Whole stereo frames only; a torn tail (never seen from the HAL)
         * is dropped rather than shifting the channels. */
        n -= n % 4;
        if (m->pcm_frames + (size_t)n / 4 > m->pcm_cap) {
            size_t ncap = (m->pcm_frames + (size_t)n / 4) * 2;
            int16_t *p = realloc(m->pcm, ncap * 4);

            if (!p) {
                return;
            }
            m->pcm = p;
            m->pcm_cap = ncap;
        }
        memcpy(m->pcm + 2 * m->pcm_frames, buf, (size_t)n);
        m->pcm_frames += (size_t)n / 4;
        aac_encode(m, cb, user);
    }
}

void cast_media_mic_pcm(struct cast_media *m, const uint8_t *pcm, size_t size)
{
    if (!m || !size || !chan_ready(&m->in)) {
        return;
    }
    /* Raw PCM has no framing to tear, so append while the backlog is small. */
    if (m->in.pending_len + size > MIC_BACKLOG * 2) {
        chan_flush(&m->in);
        if (m->in.pending_len + size > MIC_BACKLOG * 2) {
            return;
        }
    }
    if (m->in.pending_len + size > m->in.pending_cap) {
        uint8_t *n = realloc(m->in.pending, m->in.pending_len + size);

        if (!n) {
            return;
        }
        m->in.pending = n;
        m->in.pending_cap = m->in.pending_len + size;
    }
    memcpy(m->in.pending + m->in.pending_len, pcm, size);
    m->in.pending_len += size;
    chan_flush(&m->in);
}

static bool jpeg_open(struct cast_media *m)
{
    const AVCodec *codec;

    if (m->jpeg) {
        return true;
    }
    codec = avcodec_find_decoder(AV_CODEC_ID_MJPEG);
    if (!codec) {
        return false;
    }
    m->jpeg = avcodec_alloc_context3(codec);
    m->jpeg_frame = av_frame_alloc();
    m->jpeg_pkt = av_packet_alloc();
    m->nv12 = malloc(CAST_CAMERA_WIDTH * CAST_CAMERA_HEIGHT * 3 / 2);
    if (!m->jpeg || !m->jpeg_frame || !m->jpeg_pkt || !m->nv12 ||
        avcodec_open2(m->jpeg, codec, NULL) < 0) {
        fprintf(stderr, "cast-media: mjpeg decoder unavailable\n");
        avcodec_free_context(&m->jpeg);
        return false;
    }
    return true;
}

/* Scale the decoded frame to fill CAST_CAMERA_WIDTH x HEIGHT (centre crop,
 * like a phone sensor) as NV12 into m->nv12. */
static bool to_nv12(struct cast_media *m, const AVFrame *f)
{
    const int W = CAST_CAMERA_WIDTH, H = CAST_CAMERA_HEIGHT;
    const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(f->format);
    int cw = f->width, ch = f->height, cx = 0, cy = 0;
    const uint8_t *src[4] = {0};
    uint8_t *dst[4] = {m->nv12, m->nv12 + W * H, NULL, NULL};
    int dst_stride[4] = {W, W, 0, 0};

    if (!d) {
        return false;
    }
    if ((int64_t)cw * H > (int64_t)ch * W) {
        cw = (int)((int64_t)ch * W / H);
        cx = (f->width - cw) / 2;
    } else {
        ch = (int)((int64_t)cw * H / W);
        cy = (f->height - ch) / 2;
    }
    cx &= ~((1 << d->log2_chroma_w) - 1);
    cy &= ~((1 << d->log2_chroma_h) - 1);
    /* Crop by offsetting the planes; mjpeg output is planar 8-bit YUV. */
    for (int p = 0; p < 4 && f->data[p]; p++) {
        int sx = p == 0 || p == 3 ? cx : cx >> d->log2_chroma_w;
        int sy = p == 0 || p == 3 ? cy : cy >> d->log2_chroma_h;

        src[p] = f->data[p] + (size_t)sy * f->linesize[p] + sx;
    }
    m->sws = sws_getCachedContext(m->sws, cw, ch, f->format, W, H, AV_PIX_FMT_NV12,
                                  SWS_BILINEAR, NULL, NULL, NULL);
    if (!m->sws) {
        return false;
    }
    sws_scale(m->sws, src, f->linesize, 0, ch, dst, dst_stride);
    return true;
}

void cast_media_camera_jpeg(struct cast_media *m, const uint8_t *jpeg, size_t size)
{
    struct cast_camera_header hdr;

    if (!m || !size || !chan_ready(&m->cam) || !jpeg_open(m)) {
        return;
    }
    /* Skip the decode while the guest is still reading the previous frame. */
    if (!chan_flush(&m->cam)) {
        return;
    }
    if (av_new_packet(m->jpeg_pkt, (int)size) < 0) {
        return;
    }
    memcpy(m->jpeg_pkt->data, jpeg, size);
    if (avcodec_send_packet(m->jpeg, m->jpeg_pkt) < 0) {
        av_packet_unref(m->jpeg_pkt);
        return;
    }
    av_packet_unref(m->jpeg_pkt);
    if (avcodec_receive_frame(m->jpeg, m->jpeg_frame) < 0) {
        return;
    }
    if (to_nv12(m, m->jpeg_frame)) {
        memset(&hdr, 0, sizeof(hdr));
        hdr.magic = CAST_CAMERA_MAGIC;
        hdr.format = CAST_CAMERA_FMT_NV12;
        hdr.width = CAST_CAMERA_WIDTH;
        hdr.height = CAST_CAMERA_HEIGHT;
        hdr.size = CAST_CAMERA_WIDTH * CAST_CAMERA_HEIGHT * 3 / 2;
        hdr.pts_us = now_us();
        chan_write(&m->cam, &hdr, sizeof(hdr), m->nv12, hdr.size);
    }
    av_frame_unref(m->jpeg_frame);
}
