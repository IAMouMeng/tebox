/* Host audio encoder for the guest Codec2 HAL (tebox.aenc / aenc.sock).
 *
 * Guest sends PCM s16le; we encode with FFmpeg (AAC ADTS / Opus / FLAC).
 */
#include "cast/aenc.h"
#include "cast/protocol.h"

#include <libavcodec/avcodec.h>
#include <libavutil/channel_layout.h>
#include <libavutil/frame.h>
#include <libavutil/opt.h>

#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

#define MAX_PAYLOAD (1u << 20)
#define MAX_SESSIONS 4
#define PCM_CAP (64 * 1024)

struct aenc_session {
    uint32_t id;
    bool used;
    uint32_t codec; /* CAST_AENC_* */
    uint32_t sample_rate;
    uint32_t channels;
    int64_t bitrate;
    enum AVSampleFormat sample_fmt;
    bool planar;
    bool wrap_adts;
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *pkt;
    int16_t *pcm;
    size_t pcm_samples; /* interleaved samples queued */
    int64_t samples_out;
};

struct cast_aenc {
    char path[sizeof(((struct sockaddr_un *)0)->sun_path)];
    pthread_t thread;
    volatile bool run;
    int fd;
    struct aenc_session sessions[MAX_SESSIONS];
};

static int dial(const char *path)
{
    struct sockaddr_un addr = { .sun_family = AF_UNIX };
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);

    if (fd < 0) {
        return -1;
    }
    snprintf(addr.sun_path, sizeof(addr.sun_path), "%s", path);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr))) {
        close(fd);
        return -1;
    }
    return fd;
}

static bool read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;

    while (n) {
        ssize_t r = recv(fd, p, n, 0);
        if (r < 0 && errno == EINTR) {
            continue;
        }
        if (r <= 0) {
            return false;
        }
        p += r;
        n -= (size_t)r;
    }
    return true;
}

static bool write_full(int fd, const void *buf, size_t n)
{
    const uint8_t *p = buf;

    while (n) {
        ssize_t w = send(fd, p, n, 0);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return false;
        }
        p += w;
        n -= (size_t)w;
    }
    return true;
}

static void adts_header(uint8_t h[7], size_t payload, int sample_rate, int channels)
{
    const size_t len = payload + 7;
    const int profile = 1; /* AAC-LC */
    int rate_index = 3;    /* 48000 */

    static const int rates[] = {96000, 88200, 64000, 48000, 44100, 32000,
                                24000, 22050, 16000, 12000, 11025, 8000};
    for (int i = 0; i < (int)(sizeof(rates) / sizeof(rates[0])); i++) {
        if (rates[i] == sample_rate) {
            rate_index = i;
            break;
        }
    }
    if (channels < 1) {
        channels = 2;
    }
    h[0] = 0xff;
    h[1] = 0xf1;
    h[2] = (uint8_t)((profile << 6) | (rate_index << 2) | (channels >> 2));
    h[3] = (uint8_t)(((channels & 3) << 6) | ((len >> 11) & 3));
    h[4] = (uint8_t)((len >> 3) & 0xff);
    h[5] = (uint8_t)(((len & 7) << 5) | 0x1f);
    h[6] = 0xfc;
}

static const char *codec_name(uint32_t codec)
{
    switch (codec) {
    case CAST_AENC_OPUS:
        return "opus";
    case CAST_AENC_FLAC:
        return "flac";
    default:
        return "aac";
    }
}

static void end_session(struct aenc_session *s)
{
    if (!s || !s->used) {
        return;
    }
    avcodec_free_context(&s->ctx);
    av_frame_free(&s->frame);
    av_packet_free(&s->pkt);
    free(s->pcm);
    memset(s, 0, sizeof(*s));
}

static void end_all(struct cast_aenc *a)
{
    for (unsigned i = 0; i < MAX_SESSIONS; i++) {
        end_session(&a->sessions[i]);
    }
}

static struct aenc_session *find_session(struct cast_aenc *a, uint32_t id)
{
    for (unsigned i = 0; i < MAX_SESSIONS; i++) {
        if (a->sessions[i].used && a->sessions[i].id == id) {
            return &a->sessions[i];
        }
    }
    return NULL;
}

static const AVCodec *find_encoder(uint32_t codec)
{
    if (codec == CAST_AENC_OPUS) {
        const AVCodec *c = avcodec_find_encoder_by_name("libopus");
        if (c) {
            return c;
        }
        return avcodec_find_encoder_by_name("opus");
    }
    if (codec == CAST_AENC_FLAC) {
        return avcodec_find_encoder_by_name("flac");
    }
    return avcodec_find_encoder_by_name("aac");
}

static enum AVSampleFormat pick_fmt(const AVCodec *codec, uint32_t aenc_codec)
{
    if (!codec || !codec->sample_fmts) {
        return aenc_codec == CAST_AENC_FLAC ? AV_SAMPLE_FMT_S16 : AV_SAMPLE_FMT_FLTP;
    }
    for (int i = 0; codec->sample_fmts[i] != AV_SAMPLE_FMT_NONE; i++) {
        enum AVSampleFormat f = codec->sample_fmts[i];
        if (f == AV_SAMPLE_FMT_S16 || f == AV_SAMPLE_FMT_FLTP || f == AV_SAMPLE_FMT_FLT) {
            return f;
        }
    }
    return codec->sample_fmts[0];
}

static bool open_codec(struct aenc_session *s)
{
    const AVCodec *codec = find_encoder(s->codec);
    AVChannelLayout layout;

    if (!codec) {
        fprintf(stderr, "aenc: no %s encoder\n", codec_name(s->codec));
        return false;
    }
    s->ctx = avcodec_alloc_context3(codec);
    s->frame = av_frame_alloc();
    s->pkt = av_packet_alloc();
    if (!s->ctx || !s->frame || !s->pkt) {
        return false;
    }
    s->sample_fmt = pick_fmt(codec, s->codec);
    s->planar = av_sample_fmt_is_planar(s->sample_fmt);
    s->wrap_adts = (s->codec == CAST_AENC_AAC);
    s->ctx->sample_rate = (int)s->sample_rate;
    s->ctx->sample_fmt = s->sample_fmt;
    s->ctx->bit_rate = s->bitrate > 0 ? s->bitrate : 128000;
    s->ctx->time_base = (AVRational){1, (int)s->sample_rate};
    av_channel_layout_default(&layout, (int)s->channels);
    av_channel_layout_copy(&s->ctx->ch_layout, &layout);
    if (s->codec == CAST_AENC_OPUS) {
        s->ctx->compression_level = 10;
    }
    if (avcodec_open2(s->ctx, codec, NULL) < 0) {
        fprintf(stderr, "aenc: open %s failed\n", codec_name(s->codec));
        return false;
    }
    if (s->ctx->frame_size <= 0) {
        /* FLAC may report 0; use 1024 samples. */
        s->ctx->frame_size = 1024;
    }
    s->frame->nb_samples = s->ctx->frame_size;
    s->frame->format = s->sample_fmt;
    s->frame->sample_rate = (int)s->sample_rate;
    av_channel_layout_copy(&s->frame->ch_layout, &s->ctx->ch_layout);
    if (av_frame_get_buffer(s->frame, 0) < 0) {
        return false;
    }
    s->pcm = calloc(PCM_CAP, sizeof(int16_t));
    return s->pcm != NULL;
}

static bool send_out(struct cast_aenc *a, const struct cast_aenc_out *out,
                     const uint8_t *data, size_t len)
{
    struct cast_aenc_hdr hdr = {
        .magic = CAST_AENC_MAGIC,
        .type = CAST_AENC_BITSTREAM,
        .size = (uint32_t)(sizeof(*out) + len),
    };

    return write_full(a->fd, &hdr, sizeof(hdr)) && write_full(a->fd, out, sizeof(*out)) &&
           (!len || write_full(a->fd, data, len));
}

static void fill_frame(struct aenc_session *s, int fs, int ch)
{
    if (s->sample_fmt == AV_SAMPLE_FMT_S16) {
        int16_t *dst = (int16_t *)s->frame->data[0];
        memcpy(dst, s->pcm, (size_t)(fs * ch) * sizeof(int16_t));
        return;
    }
    if (s->planar) {
        for (int c = 0; c < ch; c++) {
            float *dst = (float *)s->frame->data[c];
            for (int i = 0; i < fs; i++) {
                dst[i] = s->pcm[i * ch + c] / 32768.0f;
            }
        }
        return;
    }
    float *dst = (float *)s->frame->data[0];
    for (int i = 0; i < fs * ch; i++) {
        dst[i] = s->pcm[i] / 32768.0f;
    }
}

/* Encode queued PCM; return one encoded frame (or empty) so each PCM msg gets
 * exactly one BITSTREAM reply for the guest. */
static void encode_one(struct cast_aenc *a, struct aenc_session *s,
                       const struct cast_aenc_pcm *req)
{
    const int fs = s->ctx->frame_size;
    const int ch = (int)s->channels;
    struct cast_aenc_out out = {
        .session = s->id,
        .index = req->index,
        .pts_us = req->pts_us,
        .status = 0,
        .flags = 0,
    };

    while ((int)s->pcm_samples >= fs * ch) {
        if (av_frame_make_writable(s->frame) < 0) {
            break;
        }
        fill_frame(s, fs, ch);
        memmove(s->pcm, s->pcm + fs * ch,
                (s->pcm_samples - (size_t)(fs * ch)) * sizeof(int16_t));
        s->pcm_samples -= (size_t)(fs * ch);
        s->frame->pts = s->samples_out;
        s->samples_out += fs;
        if (avcodec_send_frame(s->ctx, s->frame) < 0) {
            continue;
        }
        if (avcodec_receive_packet(s->ctx, s->pkt) == 0) {
            if (s->wrap_adts) {
                uint8_t *adts = malloc((size_t)s->pkt->size + 7);

                if (adts) {
                    adts_header(adts, (size_t)s->pkt->size, (int)s->sample_rate, ch);
                    memcpy(adts + 7, s->pkt->data, (size_t)s->pkt->size);
                    send_out(a, &out, adts, (size_t)s->pkt->size + 7);
                    free(adts);
                } else {
                    out.status = -1;
                    send_out(a, &out, NULL, 0);
                }
            } else {
                send_out(a, &out, s->pkt->data, (size_t)s->pkt->size);
            }
            av_packet_unref(s->pkt);
            return;
        }
    }
    send_out(a, &out, NULL, 0);
}

static void configure(struct cast_aenc *a, const struct cast_aenc_config *c, uint32_t nbytes)
{
    struct aenc_session *s = find_session(a, c->session);
    uint32_t codec = CAST_AENC_AAC;

    if (nbytes >= sizeof(struct cast_aenc_config)) {
        codec = c->codec;
    }
    if (codec > CAST_AENC_FLAC) {
        codec = CAST_AENC_AAC;
    }

    if (!s) {
        for (unsigned i = 0; i < MAX_SESSIONS && !s; i++) {
            if (!a->sessions[i].used) {
                s = &a->sessions[i];
            }
        }
        if (!s) {
            s = &a->sessions[0];
            fprintf(stderr, "aenc: evicting session %u\n", s->id);
            end_session(s);
        }
    } else {
        end_session(s);
    }
    s->used = true;
    s->id = c->session;
    s->codec = codec;
    s->sample_rate = c->sample_rate ? c->sample_rate : 48000;
    s->channels = c->channels ? c->channels : 2;
    s->bitrate = c->bitrate ? c->bitrate : 128000;
    if (!open_codec(s)) {
        end_session(s);
        fprintf(stderr, "aenc: session %u open failed\n", c->session);
        return;
    }
    fprintf(stderr, "aenc: session %u %s %u Hz ch=%u %u kbit/s\n", c->session,
            codec_name(s->codec), s->sample_rate, s->channels,
            (unsigned)(s->bitrate / 1000));
}

static void encode_pcm(struct cast_aenc *a, const struct cast_aenc_pcm *req,
                       const uint8_t *pcm, uint32_t nbytes)
{
    struct aenc_session *s = find_session(a, req->session);
    size_t samples;

    if (!s || !s->ctx || nbytes < 2) {
        struct cast_aenc_out out = {
            .session = req->session,
            .index = req->index,
            .pts_us = req->pts_us,
            .status = -1,
            .flags = 0,
        };

        send_out(a, &out, NULL, 0);
        return;
    }
    samples = nbytes / sizeof(int16_t);
    if (s->pcm_samples + samples > PCM_CAP) {
        s->pcm_samples = 0;
    }
    memcpy(s->pcm + s->pcm_samples, pcm, samples * sizeof(int16_t));
    s->pcm_samples += samples;
    encode_one(a, s, req);
}

static void *aenc_thread(void *opaque)
{
    struct cast_aenc *a = opaque;
    uint8_t *payload = malloc(MAX_PAYLOAD);

    while (a->run && payload) {
        struct cast_aenc_hdr hdr;

        if (a->fd < 0) {
            a->fd = dial(a->path);
            if (a->fd < 0) {
                usleep(500 * 1000);
                continue;
            }
            fprintf(stderr, "aenc: connected %s\n", a->path);
        }
        if (!read_full(a->fd, &hdr, sizeof(hdr)) || hdr.magic != CAST_AENC_MAGIC ||
            hdr.size > MAX_PAYLOAD ||
            (hdr.size && !read_full(a->fd, payload, hdr.size))) {
            close(a->fd);
            a->fd = -1;
            end_all(a);
            continue;
        }
        if (hdr.type == CAST_AENC_CONFIG && hdr.size >= 16) {
            configure(a, (const struct cast_aenc_config *)payload, hdr.size);
        } else if (hdr.type == CAST_AENC_PCM && hdr.size >= sizeof(struct cast_aenc_pcm)) {
            const struct cast_aenc_pcm *req = (const struct cast_aenc_pcm *)payload;
            uint32_t body = hdr.size - sizeof(*req);

            if (req->nbytes > body) {
                continue;
            }
            encode_pcm(a, req, payload + sizeof(*req), req->nbytes);
        } else if (hdr.type == CAST_AENC_CLOSE && hdr.size >= sizeof(uint32_t)) {
            uint32_t id;
            struct aenc_session *s;

            memcpy(&id, payload, sizeof(id));
            s = find_session(a, id);
            if (s) {
                end_session(s);
            }
        }
    }
    free(payload);
    return NULL;
}

struct cast_aenc *cast_aenc_start(const char *dir)
{
    struct cast_aenc *a = calloc(1, sizeof(*a));

    if (!a || !dir) {
        free(a);
        return NULL;
    }
    snprintf(a->path, sizeof(a->path), "%s/aenc.sock", dir);
    a->fd = -1;
    a->run = true;
    if (pthread_create(&a->thread, NULL, aenc_thread, a)) {
        free(a);
        return NULL;
    }
    return a;
}

void cast_aenc_stop(struct cast_aenc *a)
{
    if (!a) {
        return;
    }
    a->run = false;
    if (a->fd >= 0) {
        shutdown(a->fd, SHUT_RDWR);
    }
    pthread_join(a->thread, NULL);
    if (a->fd >= 0) {
        close(a->fd);
    }
    end_all(a);
    free(a);
}
