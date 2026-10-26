/* Live cast viewer with input: tebox-cast TCP stream → libavcodec → SDL2.
 *
 * With --id the viewer goes through tebox-api, one endpoint per kind:
 *   video   "TEBOX-CAST/1 <id>" on --port (9901), H.264 records only
 *   audio   "TEBOX-AUDIO/1 <id>" on --audio-port (9904): AAC records played
 *           through SDL audio; --mic sends microphone PCM records back
 *   camera  "TEBOX-CAMERA/1 <id>" on --control-port (9905): --camera JPEGs
 *   input   JSON datagrams to UDP --control-port (each event carries "id"
 *           and "seq"); guest clipboard changes come back as datagrams on
 *           the same socket.
 * Without --id it reads a bare tebox-cast port, where video, AAC and the
 * mic/camera uplink share one connection. Connections reconnect if the API
 * restarts.
 */
#include <SDL.h>
#include <libavcodec/avcodec.h>
#include <libavdevice/avdevice.h>
#include <libavformat/avformat.h>
#include <libswscale/swscale.h>

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

/* tebox-cast encoded stream: [u32 be length][u8 codec][u8 flags]
 * [u64 be pts_us][payload], both directions. */
#include "cast/protocol.h"

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0 /* macOS: SO_NOSIGPIPE is set in dial() */
#endif

/* Playback queue cap: 250 ms of s16 stereo at 48 kHz. */
#define AUDIO_MAX_QUEUED (CAST_AUDIO_RATE * 2 * 2 / 4)
/* Microphone chunks: 10 ms minimum, 20 ms maximum of s16 mono. */
#define MIC_MIN_BYTES (CAST_AUDIO_RATE * 2 / 100)
#define MIC_MAX_BYTES (CAST_AUDIO_RATE * 2 / 50)
#define CAMERA_MAX_W 1280
#define CAMERA_MAX_H 720
#define CAMERA_FRAME_US (1000000 / 30)

/* One TCP connection to the API (or a bare tebox-cast port). fd is
 * replaced or closed only under lock, so uploaders never write to a closed
 * (or reused) descriptor. A reader thread owns reconnects, except for
 * self_dial links, which nothing reads: senders redial them. */
struct link {
    const char *proto;    /* handshake, e.g. "TEBOX-CAST/1" */
    int port;
    int fd;
    SDL_mutex *lock;      /* also serialises upstream records */
    bool stalled;         /* under lock: last record was dropped */
    bool self_dial;
    uint32_t retry_at;    /* self_dial: next dial attempt (SDL ticks) */
};

struct viewer {
    const char *host;
    const char *id;
    struct link video;
    struct link audio_link;  /* --id: AAC down, microphone up */
    struct link camera_link; /* --id: camera up */
    struct link *mic_up;     /* where microphone records go */
    struct link *camera_up;  /* where camera records go */
    bool audio;           /* play AAC records */
    SDL_AudioDeviceID mic_dev;
    const char *camera;
    int ctrl_fd;          /* connected UDP socket to the input endpoint */
    uint64_t seq;
    SDL_mutex *lock;
    AVFrame *latest;      /* decoded, owned under lock */
    bool fresh;
    bool quit;
    int frame_w, frame_h;
    uint64_t frames;
};

static int read_full(int fd, void *buf, size_t n)
{
    uint8_t *p = buf;
    size_t off = 0;

    while (off < n) {
        ssize_t r = recv(fd, p + off, n - off, 0);
        if (r == 0) {
            return 0;
        }
        if (r < 0) {
            if (errno == EINTR) {
                continue;
            }
            return -1;
        }
        off += (size_t)r;
    }
    return 1;
}

static int write_full(int fd, const char *buf, size_t n)
{
    size_t off = 0;

    while (off < n) {
        ssize_t w = send(fd, buf + off, n - off, 0);
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w <= 0) {
            return -1;
        }
        off += (size_t)w;
    }
    return 0;
}

static int dial(const char *host, int port)
{
    struct addrinfo hints = {0}, *res = NULL, *ai;
    char service[16];
    int fd = -1, one = 1;

    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &res) != 0) {
        return -1;
    }
    for (ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol);
        if (fd < 0) {
            continue;
        }
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) {
            break;
        }
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd >= 0) {
        setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
#ifdef SO_NOSIGPIPE
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof(one));
#endif
    }
    return fd;
}

static int dial_handshake(const char *host, int port, const char *proto,
                          const char *id)
{
    char line[256];
    int fd = dial(host, port);

    if (fd < 0 || !id) {
        return fd;
    }
    snprintf(line, sizeof(line), "%s %s\n", proto, id);
    if (write_full(fd, line, strlen(line)) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static uint32_t r32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static void w32be(uint8_t *p, uint32_t x)
{
    p[0] = (uint8_t)(x >> 24);
    p[1] = (uint8_t)(x >> 16);
    p[2] = (uint8_t)(x >> 8);
    p[3] = (uint8_t)x;
}

static uint64_t now_us(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000u + (uint64_t)ts.tv_nsec / 1000u;
}

static void set_link_fd(struct link *l, int fd)
{
    SDL_LockMutex(l->lock);
    if (l->fd >= 0) {
        close(l->fd);
    }
    l->fd = fd;
    SDL_UnlockMutex(l->lock);
}

/* Reader threads: drop the connection, wait a second and dial again. */
static void link_redial(struct viewer *v, struct link *l, const char *what)
{
    if (l->fd >= 0) {
        set_link_fd(l, -1);
        fprintf(stderr, "cast-view: %s lost, reconnecting\n", what);
    }
    SDL_Delay(1000);
    set_link_fd(l, dial_handshake(v->host, l->port, l->proto, v->id));
}

/* One cast record into *body (grown as needed, with decoder padding).
 * Returns its length, or 0 when the connection failed or sent garbage. */
static uint32_t read_record(int fd, uint8_t **body, size_t *cap)
{
    uint8_t lenbuf[4];
    uint32_t length;

    if (fd < 0 || read_full(fd, lenbuf, 4) <= 0) {
        return 0;
    }
    length = r32be(lenbuf);
    if (length < CAST_ENC_HDR_SIZE || length > 32u * 1024u * 1024u) {
        fprintf(stderr, "cast-view: bad length %u\n", length);
        return 0;
    }
    if (length + AV_INPUT_BUFFER_PADDING_SIZE > *cap) {
        uint8_t *nbuf = realloc(*body, length + AV_INPUT_BUFFER_PADDING_SIZE);

        if (!nbuf) {
            return 0;
        }
        *body = nbuf;
        *cap = length + AV_INPUT_BUFFER_PADDING_SIZE;
    }
    if (read_full(fd, *body, length) <= 0) {
        return 0;
    }
    memset(*body + length, 0, AV_INPUT_BUFFER_PADDING_SIZE);
    return length;
}

/* Non-blocking send of p[0..n) until done or the deadline passes. A server
 * that does not read the uplink must not stall the caller forever. */
static int send_until(int fd, const uint8_t *p, size_t n, uint32_t deadline,
                      bool *started)
{
    size_t off = 0;

    while (off < n) {
        ssize_t w = send(fd, p + off, n - off, MSG_DONTWAIT | MSG_NOSIGNAL);

        if (w > 0) {
            off += (size_t)w;
            *started = true;
            continue;
        }
        if (w < 0 && errno == EINTR) {
            continue;
        }
        if (w < 0 && (errno == EAGAIN || errno == EWOULDBLOCK) &&
            !SDL_TICKS_PASSED(SDL_GetTicks(), deadline)) {
            struct pollfd pfd = { .fd = fd, .events = POLLOUT };

            (void)poll(&pfd, 1, 10);
            continue;
        }
        return -1;
    }
    return 0;
}

/* One upstream record, written whole under the link lock (mic and camera
 * may share a link from different threads). A record that cannot start
 * within 500 ms is dropped; one that stops half way would desync the
 * stream, so the socket is dropped and reconnected. */
static void send_record(struct viewer *v, struct link *l, uint8_t codec,
                        const void *payload, size_t n)
{
    uint8_t hdr[4 + CAST_ENC_HDR_SIZE];
    uint64_t pts = now_us();
    uint32_t deadline = SDL_GetTicks() + 500;
    bool started = false;
    int i;

    if (n + CAST_ENC_HDR_SIZE > CAST_UPLINK_MAX) {
        return;
    }
    w32be(hdr, (uint32_t)(n + CAST_ENC_HDR_SIZE));
    hdr[4] = codec;
    hdr[5] = 0;
    for (i = 0; i < 8; i++) {
        hdr[6 + i] = (uint8_t)(pts >> (56 - 8 * i));
    }
    SDL_LockMutex(l->lock);
    if (l->fd < 0 && l->self_dial && SDL_TICKS_PASSED(SDL_GetTicks(), l->retry_at)) {
        l->fd = dial_handshake(v->host, l->port, l->proto, v->id);
        l->retry_at = SDL_GetTicks() + 1000;
    }
    if (l->fd >= 0) {
        if (send_until(l->fd, hdr, sizeof(hdr), deadline, &started) < 0 ||
            send_until(l->fd, payload, n, deadline, &started) < 0) {
            if (started || l->self_dial) {
                fprintf(stderr, "cast-view: %s uplink failed, reconnecting\n",
                        l->proto);
                if (l->self_dial) {
                    close(l->fd);
                    l->fd = -1;
                } else {
                    shutdown(l->fd, SHUT_RDWR); /* its reader redials */
                }
            } else if (!l->stalled) {
                fprintf(stderr, "cast-view: uplink stalled, dropping records\n");
            }
            l->stalled = true;
        } else {
            l->stalled = false;
        }
    }
    SDL_UnlockMutex(l->lock);
}

/* AAC playback state, owned by the video thread. */
struct audio_out {
    AVCodecContext *dec;
    AVPacket *pkt;
    AVFrame *frame;
    SDL_AudioDeviceID dev;
    bool failed;          /* logged once; audio stays off */
    bool bad_format;
    int16_t *pcm;
    size_t pcm_cap;       /* in stereo sample frames */
};

/* Lazily opened on the first AAC record. */
static bool audio_open(struct audio_out *a)
{
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_AAC);
    SDL_AudioSpec want = {0};

    if (a->dev) {
        return true;
    }
    if (a->failed) {
        return false;
    }
    a->failed = true;
    a->dec = codec ? avcodec_alloc_context3(codec) : NULL;
    a->pkt = av_packet_alloc();
    a->frame = av_frame_alloc();
    if (!a->dec || !a->pkt || !a->frame || avcodec_open2(a->dec, codec, NULL) < 0) {
        fprintf(stderr, "cast-view: no AAC decoder, audio off\n");
        return false;
    }
    want.freq = CAST_AUDIO_RATE;
    want.format = AUDIO_S16SYS;
    want.channels = 2;
    want.samples = 512;
    a->dev = SDL_OpenAudioDevice(NULL, 0, &want, NULL, 0);
    if (!a->dev) {
        fprintf(stderr, "cast-view: audio device: %s, audio off\n",
                SDL_GetError());
        return false;
    }
    SDL_PauseAudioDevice(a->dev, 0);
    a->failed = false;
    fprintf(stderr, "cast-view: audio on\n");
    return true;
}

static void audio_close(struct audio_out *a)
{
    if (a->dev) {
        SDL_CloseAudioDevice(a->dev);
    }
    avcodec_free_context(&a->dec);
    av_packet_free(&a->pkt);
    av_frame_free(&a->frame);
    free(a->pcm);
}

/* Decoded frame (planar or packed, float or s16, any channel count) to
 * interleaved s16 stereo in a->pcm; returns sample frames. */
static size_t audio_to_s16(struct audio_out *a, const AVFrame *f)
{
    enum AVSampleFormat fmt = f->format;
    enum AVSampleFormat packed = av_get_packed_sample_fmt(fmt);
    bool planar = av_sample_fmt_is_planar(fmt);
    int ch = f->ch_layout.nb_channels;
    size_t n = (size_t)f->nb_samples;

    if (ch < 1 || (packed != AV_SAMPLE_FMT_FLT && packed != AV_SAMPLE_FMT_S16)) {
        if (!a->bad_format) {
            fprintf(stderr, "cast-view: unsupported audio format %s/%d ch\n",
                    av_get_sample_fmt_name(fmt), ch);
            a->bad_format = true;
        }
        return 0;
    }
    if (n > a->pcm_cap) {
        int16_t *np = realloc(a->pcm, n * 2 * sizeof(int16_t));

        if (!np) {
            return 0;
        }
        a->pcm = np;
        a->pcm_cap = n;
    }
    for (size_t i = 0; i < n; i++) {
        for (int c = 0; c < 2; c++) {
            int src = c < ch ? c : 0; /* mono: duplicate; >2: first two */
            const uint8_t *plane = f->extended_data[planar ? src : 0];
            size_t idx = planar ? i : i * (size_t)ch + (size_t)src;

            if (packed == AV_SAMPLE_FMT_FLT) {
                float x = ((const float *)plane)[idx];

                x = x > 1.f ? 1.f : x < -1.f ? -1.f : x;
                a->pcm[i * 2 + c] = (int16_t)(x * 32767.f);
            } else {
                a->pcm[i * 2 + c] = ((const int16_t *)plane)[idx];
            }
        }
    }
    return n;
}

/* One ADTS frame. Latency beats continuity: a backlog over 250 ms (stalled
 * device, burst after reconnect) is dropped before queuing more. */
static void play_aac(struct audio_out *a, uint8_t *data, int size)
{
    if (!audio_open(a)) {
        return;
    }
    a->pkt->data = data;
    a->pkt->size = size;
    if (avcodec_send_packet(a->dec, a->pkt) < 0) {
        return;
    }
    while (avcodec_receive_frame(a->dec, a->frame) == 0) {
        size_t n = audio_to_s16(a, a->frame);

        if (n) {
            if (SDL_GetQueuedAudioSize(a->dev) > AUDIO_MAX_QUEUED) {
                SDL_ClearQueuedAudio(a->dev);
            }
            SDL_QueueAudio(a->dev, a->pcm, (Uint32)(n * 2 * sizeof(int16_t)));
        }
        av_frame_unref(a->frame);
    }
}

/* Network + decode thread; hands the newest picture to the UI thread. */
static int video_thread(void *opaque)
{
    struct viewer *v = opaque;
    const AVCodec *codec = avcodec_find_decoder(AV_CODEC_ID_H264);
    AVCodecContext *ctx = codec ? avcodec_alloc_context3(codec) : NULL;
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();
    struct audio_out audio = {0};
    uint8_t *body = NULL;
    size_t cap = 0;

    if (!ctx || !pkt || !frame) {
        fprintf(stderr, "cast-view: no H.264 decoder\n");
        goto out;
    }
    ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    ctx->thread_count = 1; /* frame threads add a frame of latency each */
    if (avcodec_open2(ctx, codec, NULL) < 0) {
        goto out;
    }
    while (!v->quit) {
        uint32_t length = read_record(v->video.fd, &body, &cap);

        if (!length) {
            /* API or cast restarted: keep the last picture and reconnect.
             * Bare-port uploads resume once the new socket is published. */
            link_redial(v, &v->video, "video");
            avcodec_flush_buffers(ctx);
            if (audio.dec) {
                avcodec_flush_buffers(audio.dec);
            }
            continue;
        }
        if (body[0] == CAST_CODEC_AAC) {
            /* Bare tebox-cast port only; the API sends audio separately. */
            if (v->audio && !v->id) {
                play_aac(&audio, body + CAST_ENC_HDR_SIZE,
                         (int)(length - CAST_ENC_HDR_SIZE));
            }
            continue;
        }
        if (body[0] != CAST_CODEC_H264) {
            continue;
        }
        pkt->data = body + CAST_ENC_HDR_SIZE;
        pkt->size = (int)(length - CAST_ENC_HDR_SIZE);
        if (avcodec_send_packet(ctx, pkt) < 0) {
            continue;
        }
        while (avcodec_receive_frame(ctx, frame) == 0) {
            if (v->frames == 0) {
                fprintf(stderr, "cast-view: first frame %dx%d\n",
                        frame->width, frame->height);
            }
            SDL_LockMutex(v->lock);
            av_frame_unref(v->latest);
            av_frame_move_ref(v->latest, frame);
            v->fresh = true;
            v->frames++;
            SDL_UnlockMutex(v->lock);
        }
    }
out:
    free(body);
    audio_close(&audio);
    av_frame_free(&frame);
    av_packet_free(&pkt);
    avcodec_free_context(&ctx);
    v->quit = true;
    {
        SDL_Event ev = { .type = SDL_QUIT };
        SDL_PushEvent(&ev);
    }
    return 0;
}

/* --id: the API's audio endpoint, AAC records down (the microphone goes
 * up on the same connection). */
static int audio_thread(void *opaque)
{
    struct viewer *v = opaque;
    struct audio_out audio = {0};
    uint8_t *body = NULL;
    size_t cap = 0;

    while (!v->quit) {
        uint32_t length = read_record(v->audio_link.fd, &body, &cap);

        if (!length) {
            link_redial(v, &v->audio_link, "audio");
            if (audio.dec) {
                avcodec_flush_buffers(audio.dec);
            }
            continue;
        }
        if (body[0] == CAST_CODEC_AAC && v->audio) {
            play_aac(&audio, body + CAST_ENC_HDR_SIZE,
                     (int)(length - CAST_ENC_HDR_SIZE));
        }
    }
    free(body);
    audio_close(&audio);
    return 0;
}

/* Microphone uplink: drains the SDL capture queue in 10-20 ms records. */
static int mic_thread(void *opaque)
{
    struct viewer *v = opaque;
    uint8_t buf[MIC_MAX_BYTES];

    while (!v->quit) {
        Uint32 avail = SDL_GetQueuedAudioSize(v->mic_dev);
        Uint32 n;

        if (avail > MIC_MAX_BYTES * 10) {
            /* Over 200 ms behind (uplink stalled): drop, stay live. */
            SDL_ClearQueuedAudio(v->mic_dev);
            continue;
        }
        if (avail < MIC_MIN_BYTES) {
            SDL_Delay(5);
            continue;
        }
        n = SDL_DequeueAudio(v->mic_dev, buf,
                             avail < sizeof(buf) ? avail & ~1u : sizeof(buf));
        if (n) {
            send_record(v, v->mic_up, CAST_CODEC_PCM_S16LE, buf, n);
        }
    }
    return 0;
}

/* Camera uplink state, owned by the camera thread. */
struct camera {
    struct viewer *v;
    bool live;            /* avfoundation device: no pacing, no looping */
    AVCodecContext *enc;  /* mjpeg, recreated when the output size changes */
    struct SwsContext *sws;
    int sws_full;         /* source range the sws context was set up for */
    AVFrame *scaled;
    AVPacket *jpeg;
    AVPacket *last;       /* last JPEG sent, repeated for still images */
    AVRational tb;
    int64_t first_ts;     /* first timestamp of this pass (files) */
    uint64_t pass_start;
    uint64_t next_send;   /* 30 fps send grid */
    int64_t encoded;
};

static int camera_interrupt(void *opaque)
{
    return ((struct viewer *)opaque)->quit;
}

/* avfoundation:N opens macOS camera N (1280x720@30 if offered, else what
 * the device picks); anything else is a media file. */
static AVFormatContext *camera_open(struct viewer *v, const char *src, bool live)
{
    AVFormatContext *fmt = NULL;
    int attempt;

    if (!live) {
        fmt = avformat_alloc_context();
        if (!fmt) {
            return NULL;
        }
        fmt->interrupt_callback.callback = camera_interrupt;
        fmt->interrupt_callback.opaque = v;
        if (avformat_open_input(&fmt, src, NULL, NULL) < 0) {
            return NULL;
        }
        return fmt;
    }
    for (attempt = 0; attempt < 3; attempt++) {
        const AVInputFormat *in = av_find_input_format("avfoundation");
        AVDictionary *opts = NULL;

        if (!in) {
            fprintf(stderr, "cast-view: FFmpeg has no avfoundation device\n");
            return NULL;
        }
        /* 0: 1280x720@30, 1: device size @30, 2: device defaults. */
        if (attempt < 2) {
            av_dict_set(&opts, "framerate", "30", 0);
            av_dict_set(&opts, "pixel_format", "nv12", 0);
        }
        if (attempt < 1) {
            av_dict_set(&opts, "video_size", "1280x720", 0);
        }
        fmt = avformat_alloc_context();
        if (!fmt) {
            av_dict_free(&opts);
            return NULL;
        }
        fmt->interrupt_callback.callback = camera_interrupt;
        fmt->interrupt_callback.opaque = v;
        if (avformat_open_input(&fmt, src + strlen("avfoundation:"), in,
                                &opts) == 0) {
            av_dict_free(&opts);
            return fmt;
        }
        av_dict_free(&opts);
    }
    return NULL;
}

/* (Re)create the mjpeg encoder and the scaled frame for an output size. */
static int camera_setup_encoder(struct camera *c, int w, int h)
{
    const AVCodec *codec = avcodec_find_encoder(AV_CODEC_ID_MJPEG);

    avcodec_free_context(&c->enc);
    av_frame_unref(c->scaled);
    c->enc = codec ? avcodec_alloc_context3(codec) : NULL;
    if (!c->enc) {
        return -1;
    }
    c->enc->width = w;
    c->enc->height = h;
    /* yuv420p + full range is what yuvj420p meant, without the deprecation. */
    c->enc->pix_fmt = AV_PIX_FMT_YUV420P;
    c->enc->color_range = AVCOL_RANGE_JPEG;
    c->enc->time_base = (AVRational){ 1, 30 };
    c->enc->flags |= AV_CODEC_FLAG_QSCALE;
    c->enc->global_quality = FF_QP2LAMBDA * 5;
    if (avcodec_open2(c->enc, codec, NULL) < 0) {
        avcodec_free_context(&c->enc);
        return -1;
    }
    c->scaled->format = AV_PIX_FMT_YUV420P;
    c->scaled->color_range = AVCOL_RANGE_JPEG;
    c->scaled->width = w;
    c->scaled->height = h;
    return av_frame_get_buffer(c->scaled, 0);
}

/* yuvj* sources become their yuv* twin + full range for swscale. */
static enum AVPixelFormat camera_src_format(enum AVPixelFormat f, int *full)
{
    switch (f) {
    case AV_PIX_FMT_YUVJ420P: *full = 1; return AV_PIX_FMT_YUV420P;
    case AV_PIX_FMT_YUVJ422P: *full = 1; return AV_PIX_FMT_YUV422P;
    case AV_PIX_FMT_YUVJ444P: *full = 1; return AV_PIX_FMT_YUV444P;
    case AV_PIX_FMT_YUVJ440P: *full = 1; return AV_PIX_FMT_YUV440P;
    case AV_PIX_FMT_YUVJ411P: *full = 1; return AV_PIX_FMT_YUV411P;
    default: return f;
    }
}

/* Pace (files), rate-limit to 30 fps, scale to fit 1280x720, encode, send. */
static void camera_frame(struct camera *c, AVFrame *f)
{
    int64_t ts = f->best_effort_timestamp;
    int w = f->width, h = f->height, full = f->color_range == AVCOL_RANGE_JPEG;
    enum AVPixelFormat sfmt = camera_src_format(f->format, &full);
    struct SwsContext *sws;
    uint64_t now;

    if (!c->live && ts != AV_NOPTS_VALUE) {
        int64_t due;

        if (c->first_ts == AV_NOPTS_VALUE) {
            c->first_ts = ts;
        }
        due = (int64_t)c->pass_start +
              av_rescale_q(ts - c->first_ts, c->tb, AV_TIME_BASE_Q);
        while (!c->v->quit && (int64_t)now_us() < due) {
            int64_t ms = (due - (int64_t)now_us()) / 1000;

            SDL_Delay(ms > 10 ? 10 : ms > 0 ? (Uint32)ms : 1);
        }
    }
    /* A fixed 30 fps grid with 5 ms slack: a 30 fps source with jitter is
     * not halved, a faster one is thinned to 30. */
    now = now_us();
    if (c->next_send && now + 5000 < c->next_send) {
        return;
    }
    c->next_send = c->next_send && now < c->next_send + CAMERA_FRAME_US
                       ? c->next_send + CAMERA_FRAME_US
                       : now + CAMERA_FRAME_US;
    if (w > CAMERA_MAX_W || h > CAMERA_MAX_H) {
        if ((int64_t)w * CAMERA_MAX_H > (int64_t)h * CAMERA_MAX_W) {
            h = (int)((int64_t)h * CAMERA_MAX_W / w);
            w = CAMERA_MAX_W;
        } else {
            w = (int)((int64_t)w * CAMERA_MAX_H / h);
            h = CAMERA_MAX_H;
        }
    }
    w &= ~1;
    h &= ~1;
    if (w < 2 || h < 2) {
        return;
    }
    if ((!c->enc || c->enc->width != w || c->enc->height != h) &&
        camera_setup_encoder(c, w, h) < 0) {
        fprintf(stderr, "cast-view: mjpeg encoder setup failed\n");
        return;
    }
    sws = sws_getCachedContext(c->sws, f->width, f->height, sfmt, w, h,
                               AV_PIX_FMT_YUV420P, SWS_BILINEAR,
                               NULL, NULL, NULL);
    if (!sws || av_frame_make_writable(c->scaled) < 0) {
        c->sws = sws;
        return;
    }
    if (sws != c->sws || full != c->sws_full) {
        /* Output is full-range (JPEG); input range comes from the frame. */
        sws_setColorspaceDetails(sws, sws_getCoefficients(SWS_CS_DEFAULT), full,
                                 sws_getCoefficients(SWS_CS_DEFAULT), 1,
                                 0, 1 << 16, 1 << 16);
        c->sws_full = full;
    }
    c->sws = sws;
    sws_scale(c->sws, (const uint8_t *const *)f->data, f->linesize, 0,
              f->height, c->scaled->data, c->scaled->linesize);
    c->scaled->pts = c->encoded++;
    c->scaled->quality = c->enc->global_quality;
    if (avcodec_send_frame(c->enc, c->scaled) < 0) {
        return;
    }
    while (avcodec_receive_packet(c->enc, c->jpeg) == 0) {
        send_record(c->v, c->v->camera_up, CAST_CODEC_MJPEG, c->jpeg->data,
                    (size_t)c->jpeg->size);
        av_packet_unref(c->last);
        av_packet_move_ref(c->last, c->jpeg);
    }
}

/* Camera uplink thread: one source, decoded and re-encoded as JPEG records.
 * Files loop forever; a single-frame source (still image) is resent at
 * 30 fps. Open failure ends only this thread. */
static int camera_thread(void *opaque)
{
    struct viewer *v = opaque;
    struct camera c = { .v = v };
    AVPacket *pkt = av_packet_alloc();
    AVFrame *frame = av_frame_alloc();

    c.live = !strncmp(v->camera, "avfoundation:", strlen("avfoundation:"));
    c.scaled = av_frame_alloc();
    c.jpeg = av_packet_alloc();
    c.last = av_packet_alloc();
    while (!v->quit && pkt && frame && c.scaled && c.jpeg && c.last) {
        AVFormatContext *fmt = camera_open(v, v->camera, c.live);
        AVCodecContext *dec = NULL;
        const AVCodec *codec = NULL;
        int idx = -1, frames = 0, ret;

        if (!fmt) {
            fprintf(stderr, "cast-view: camera %s: cannot open\n", v->camera);
            break;
        }
        if (avformat_find_stream_info(fmt, NULL) >= 0) {
            idx = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, &codec, 0);
        }
        if (idx >= 0) {
            dec = avcodec_alloc_context3(codec);
        }
        if (!dec || avcodec_parameters_to_context(dec, fmt->streams[idx]->codecpar) < 0 ||
            avcodec_open2(dec, codec, NULL) < 0) {
            fprintf(stderr, "cast-view: camera %s: no decodable video\n",
                    v->camera);
            avcodec_free_context(&dec);
            avformat_close_input(&fmt);
            break;
        }
        if (!c.encoded) {
            fprintf(stderr, "cast-view: camera %s %dx%d\n", v->camera,
                    dec->width, dec->height);
        }
        c.tb = fmt->streams[idx]->time_base;
        c.first_ts = AV_NOPTS_VALUE;
        c.pass_start = now_us();
        while (!v->quit) {
            ret = av_read_frame(fmt, pkt);
            if (ret == AVERROR(EAGAIN)) {
                SDL_Delay(2); /* avfoundation: no frame yet */
                continue;
            }
            if (ret < 0) {
                avcodec_send_packet(dec, NULL); /* drain */
            } else if (pkt->stream_index != idx) {
                av_packet_unref(pkt);
                continue;
            } else {
                avcodec_send_packet(dec, pkt);
                av_packet_unref(pkt);
            }
            while (avcodec_receive_frame(dec, frame) == 0) {
                frames++;
                camera_frame(&c, frame);
                av_frame_unref(frame);
            }
            if (ret < 0) {
                break;
            }
        }
        avcodec_free_context(&dec);
        avformat_close_input(&fmt);
        if (c.live) {
            /* Device went away: retry, giving up if it cannot reopen. */
            if (!v->quit) {
                fprintf(stderr, "cast-view: camera read failed, reopening\n");
                SDL_Delay(1000);
            }
            continue;
        }
        if (frames == 0) {
            fprintf(stderr, "cast-view: camera %s: no frames\n", v->camera);
            break;
        }
        if (frames == 1) {
            while (!v->quit && c.last->size) {
                SDL_Delay(CAMERA_FRAME_US / 1000);
                send_record(v, v->camera_up, CAST_CODEC_MJPEG, c.last->data,
                            (size_t)c.last->size);
            }
            break;
        }
    }
    av_packet_free(&pkt);
    av_frame_free(&frame);
    av_frame_free(&c.scaled);
    av_packet_free(&c.jpeg);
    av_packet_free(&c.last);
    avcodec_free_context(&c.enc);
    sws_freeContext(c.sws);
    return 0;
}

/* One event per datagram; json is an object literal starting with '{'.
 * Send errors (API restarting) are ignored: UDP has no connection to lose. */
static void send_ctrl(struct viewer *v, const char *json)
{
    size_t n = strlen(json) + strlen(v->id) + 64;
    char *dgram;

    if (v->ctrl_fd < 0 || json[0] != '{' || !(dgram = malloc(n))) {
        return;
    }
    n = (size_t)snprintf(dgram, n, "{\"id\":\"%s\",\"seq\":%llu,%s\n", v->id,
                         (unsigned long long)++v->seq, json + 1);
    if (n > 60000) {
        fprintf(stderr, "cast-view: event too large for UDP, dropped\n");
    } else {
        (void)send(v->ctrl_fd, dgram, n, 0);
    }
    free(dgram);
}

static int open_input_udp(const char *host, int port)
{
    struct addrinfo hints = {0}, *res = NULL;
    char service[16];
    int fd = -1;

    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_DGRAM;
    snprintf(service, sizeof(service), "%d", port);
    if (getaddrinfo(host, service, &hints, &res) != 0) {
        return -1;
    }
    fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) < 0) {
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    return fd;
}

/* JSON string literal (with quotes) for UTF-8 text; caller frees. */
static char *json_quote(const char *text)
{
    size_t n = strlen(text);
    char *out = malloc(n * 6 + 3);
    char *p = out;

    if (!out) {
        return NULL;
    }
    *p++ = '"';
    for (; *text; text++) {
        unsigned char c = (unsigned char)*text;

        if (c == '"' || c == '\\') {
            *p++ = '\\';
            *p++ = (char)c;
        } else if (c == '\n') {
            *p++ = '\\';
            *p++ = 'n';
        } else if (c < 0x20) {
            p += sprintf(p, "\\u%04x", c);
        } else {
            *p++ = (char)c;
        }
    }
    *p++ = '"';
    *p = '\0';
    return out;
}

static void send_text_event(struct viewer *v, const char *type,
                            const char *text, const char *extra)
{
    char *quoted = json_quote(text);
    char *json;

    if (!quoted) {
        return;
    }
    json = malloc(strlen(quoted) + 64);
    if (json) {
        sprintf(json, "{\"type\":\"%s\",\"text\":%s%s}", type, quoted, extra);
        send_ctrl(v, json);
        free(json);
    }
    free(quoted);
}

static void send_keycode(struct viewer *v, int keycode, int meta)
{
    char json[96];

    snprintf(json, sizeof(json),
             "{\"type\":\"keycode\",\"keycode\":%d,\"meta\":%d}",
             keycode, meta);
    send_ctrl(v, json);
}

/* Decode the "text" string of a {"type":"clipboard",...} line; caller frees. */
static char *parse_clipboard_line(const char *line)
{
    const char *p;
    char *out, *o;

    if (!strstr(line, "\"type\":\"clipboard\"") ||
        !(p = strstr(line, "\"text\":\""))) {
        return NULL;
    }
    p += 8;
    out = o = malloc(strlen(p) + 1);
    if (!out) {
        return NULL;
    }
    while (*p && *p != '"') {
        unsigned cp;

        if (*p != '\\') {
            *o++ = *p++;
            continue;
        }
        p++;
        switch (*p) {
        case 'n': *o++ = '\n'; p++; continue;
        case 't': *o++ = '\t'; p++; continue;
        case 'r': *o++ = '\r'; p++; continue;
        case 'b': *o++ = '\b'; p++; continue;
        case 'f': *o++ = '\f'; p++; continue;
        case 'u': break;
        case '\0': continue;
        default: *o++ = *p++; continue;
        }
        if (sscanf(p + 1, "%4x", &cp) != 1) {
            break;
        }
        p += 5;
        if (cp >= 0xd800 && cp < 0xdc00 && p[0] == '\\' && p[1] == 'u') {
            unsigned lo;

            if (sscanf(p + 2, "%4x", &lo) == 1 && lo >= 0xdc00 && lo < 0xe000) {
                cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
                p += 6;
            }
        }
        if (cp < 0x80) {
            *o++ = (char)cp;
        } else if (cp < 0x800) {
            *o++ = (char)(0xc0 | cp >> 6);
            *o++ = (char)(0x80 | (cp & 0x3f));
        } else if (cp < 0x10000) {
            *o++ = (char)(0xe0 | cp >> 12);
            *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
            *o++ = (char)(0x80 | (cp & 0x3f));
        } else {
            *o++ = (char)(0xf0 | cp >> 18);
            *o++ = (char)(0x80 | ((cp >> 12) & 0x3f));
            *o++ = (char)(0x80 | ((cp >> 6) & 0x3f));
            *o++ = (char)(0x80 | (cp & 0x3f));
        }
    }
    *o = '\0';
    return out;
}

static Uint32 clipboard_event_type;

/* Input-socket reader: guest clipboard changes become SDL user events so
 * the host clipboard is written on the main (Cocoa) thread. */
static int control_thread(void *opaque)
{
    struct viewer *v = opaque;
    int fd = v->ctrl_fd;
    char *line = NULL;
    size_t len = 0, cap = 0;

    while (!v->quit) {
        static char buf[65536];
        ssize_t n = recv(fd, buf, sizeof(buf) - 1, 0);

        if (n <= 0) {
            /* ECONNREFUSED while the API is down; keep listening. */
            SDL_Delay(200);
            continue;
        }
        if (buf[n - 1] != '\n') {
            buf[n++] = '\n';
        }
        for (ssize_t i = 0; i < n; i++) {
            if (buf[i] != '\n') {
                if (len + 2 > cap) {
                    char *nl = realloc(line, cap ? cap * 2 : 4096);

                    if (!nl) {
                        goto out;
                    }
                    line = nl;
                    cap = cap ? cap * 2 : 4096;
                }
                line[len++] = buf[i];
                continue;
            }
            line[len] = '\0';
            len = 0;
            char *text = parse_clipboard_line(line);
            if (text) {
                SDL_Event ev = { .type = clipboard_event_type };

                ev.user.data1 = text;
                SDL_PushEvent(&ev);
            } else if (strstr(line, "\"ok\":false")) {
                fprintf(stderr, "cast-view: %s\n", line);
            }
        }
    }
out:
    free(line);
    return 0;
}

static void send_touch(struct viewer *v, const char *action, float x, float y)
{
    char json[128];

    snprintf(json, sizeof(json),
             "{\"type\":\"touch\",\"action\":\"%s\",\"x\":%.5f,\"y\":%.5f}",
             action, x, y);
    send_ctrl(v, json);
}

static void send_key(struct viewer *v, const char *key)
{
    char json[96];

    snprintf(json, sizeof(json), "{\"type\":\"key\",\"key\":\"%s\"}", key);
    send_ctrl(v, json);
}

/* Letterboxed picture rectangle inside the window, in window points. */
static SDL_Rect fit_rect(SDL_Window *win, int fw, int fh)
{
    int ww, wh;
    SDL_Rect r;

    SDL_GetWindowSize(win, &ww, &wh);
    if (fw <= 0 || fh <= 0) {
        r.x = r.y = 0;
        r.w = ww;
        r.h = wh;
        return r;
    }
    if ((int64_t)ww * fh > (int64_t)wh * fw) {
        r.h = wh;
        r.w = (int)((int64_t)wh * fw / fh);
    } else {
        r.w = ww;
        r.h = (int)((int64_t)ww * fh / fw);
    }
    r.x = (ww - r.w) / 2;
    r.y = (wh - r.h) / 2;
    return r;
}

/* Cursor position in window points. Event coordinates are not used: on
 * Retina, sdl2-compat (SDL3) reports them in pixels while SDL2 used points,
 * so the global cursor minus the window origin is the only stable source. */
static void cursor_in_window(SDL_Window *win, int *x, int *y)
{
    int gx, gy, wx, wy;

    SDL_GetGlobalMouseState(&gx, &gy);
    SDL_GetWindowPosition(win, &wx, &wy);
    *x = gx - wx;
    *y = gy - wy;
}

static bool to_guest(struct viewer *v, SDL_Window *win, float *gx, float *gy)
{
    SDL_Rect r = fit_rect(win, v->frame_w, v->frame_h);
    int x, y;

    cursor_in_window(win, &x, &y);

    if (r.w <= 0 || r.h <= 0) {
        return false;
    }
    *gx = (float)(x - r.x) / (float)r.w;
    *gy = (float)(y - r.y) / (float)r.h;
    return *gx >= 0.f && *gx <= 1.f && *gy >= 0.f && *gy <= 1.f;
}

/* Android navigation keys, sent through QEMU's virtio keyboard. */
static const char *nav_key_for(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_ESCAPE:   return "back";
    case SDLK_HOME:
    case SDLK_F1:       return "home";
    case SDLK_F2:       return "menu";
    case SDLK_F3:       return "recent"; /* overview / APP_SWITCH */
    case SDLK_F5:       return "power";
    case SDLK_PAGEUP:   return "volume_up";
    case SDLK_PAGEDOWN: return "volume_down";
    default:            return NULL;
    }
}

/* Editing keys as Android KEYCODE_* values (text arrives as SDL_TEXTINPUT). */
static int android_keycode_for(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_BACKSPACE: return 67;  /* DEL */
    case SDLK_DELETE:    return 112; /* FORWARD_DEL */
    case SDLK_RETURN:
    case SDLK_KP_ENTER:  return 66;  /* ENTER */
    case SDLK_TAB:       return 61;
    case SDLK_UP:        return 19;
    case SDLK_DOWN:      return 20;
    case SDLK_LEFT:      return 21;
    case SDLK_RIGHT:     return 22;
    case SDLK_END:       return 123; /* MOVE_END */
    default:             return 0;
    }
}

/* Cmd/Ctrl shortcuts mapped to Android Ctrl+key (Cmd+V pastes the host
 * clipboard instead). */
static int android_shortcut_for(SDL_Keycode sym)
{
    switch (sym) {
    case SDLK_a: return 29;
    case SDLK_c: return 31;
    case SDLK_x: return 52;
    case SDLK_y: return 53;
    case SDLK_z: return 54;
    default:     return 0;
    }
}

#define ANDROID_META_SHIFT 0x41   /* META_SHIFT_ON | META_SHIFT_LEFT_ON */
#define ANDROID_META_CTRL  0x3000 /* META_CTRL_ON | META_CTRL_LEFT_ON */

static void usage(const char *argv0)
{
    fprintf(stderr,
            "Usage: %s [--host H] [--port P] [--id INSTANCE]\n"
            "          [--audio-port A] [--control-port C]\n"
            "          [--height PX] [--no-audio] [--mic] [--camera SRC]\n"
            "  --id          go through tebox-api: video on --port (9901),\n"
            "                audio + mic on --audio-port (9904), camera (TCP)\n"
            "                and input (UDP) on --control-port (9905); without\n"
            "                it --port is a bare tebox-cast port (video, audio\n"
            "                and uplink in one)\n"
            "  --height      initial window height in points (default 900)\n"
            "  --no-audio    do not play the guest's audio (AAC records)\n"
            "  --mic         send the default microphone to the guest\n"
            "                (48 kHz mono PCM)\n"
            "  --camera SRC  send a camera to the guest as JPEG frames:\n"
            "                avfoundation:N (macOS camera N, e.g. avfoundation:0)\n"
            "                or a video/image file, looped (30 fps max)\n"
            "Mouse: touch.  Esc / right click back, Home/F1 home, F2 menu,\n"
            "F3 recent, F5 power, PgUp/PgDn volume.  Typing goes to the\n"
            "focused field; Cmd+A/C/X/Z/Y act in the guest, Cmd+V pastes the\n"
            "host clipboard.  Clipboards sync both ways.\n",
            argv0);
}

int main(int argc, char **argv)
{
    const char *host = "127.0.0.1";
    const char *id = NULL;
    int port = 9901;
    int audio_port = 9904;
    int control_port = 9905;
    int win_h = 900;
    struct viewer v = { .ctrl_fd = -1, .audio = true };
    bool mic = false;
    SDL_Window *win;
    SDL_Renderer *ren;
    SDL_Texture *tex = NULL;
    SDL_Thread *thread, *audio_thr = NULL, *mic_thr = NULL, *camera_thr = NULL;
    bool touching = false;
    float touch_x = 0, touch_y = 0;
    uint32_t touch_tick = 0, ping_tick = 0;
    unsigned taps = 0;
    char *last_clip = NULL; /* last text synced either way, to stop echoes */
    uint32_t title_tick = 0;
    uint64_t title_frames = 0;
    int i;

    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--host") && i + 1 < argc) {
            host = argv[++i];
        } else if (!strcmp(argv[i], "--port") && i + 1 < argc) {
            port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--id") && i + 1 < argc) {
            id = argv[++i];
        } else if (!strcmp(argv[i], "--audio-port") && i + 1 < argc) {
            audio_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--control-port") && i + 1 < argc) {
            control_port = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--height") && i + 1 < argc) {
            win_h = atoi(argv[++i]);
        } else if (!strcmp(argv[i], "--no-audio")) {
            v.audio = false;
        } else if (!strcmp(argv[i], "--mic")) {
            mic = true;
        } else if (!strcmp(argv[i], "--camera") && i + 1 < argc) {
            v.camera = argv[++i];
        } else {
            usage(argv[0]);
            return !strcmp(argv[i], "--help") ? 0 : 1;
        }
    }

    v.host = host;
    v.id = id;
    v.video = (struct link){ .proto = "TEBOX-CAST/1", .port = port, .fd = -1 };
    v.audio_link = (struct link){ .proto = "TEBOX-AUDIO/1", .port = audio_port,
                                  .fd = -1 };
    v.camera_link = (struct link){ .proto = "TEBOX-CAMERA/1",
                                   .port = control_port, .fd = -1,
                                   .self_dial = true };
    v.video.lock = SDL_CreateMutex();
    v.audio_link.lock = SDL_CreateMutex();
    v.camera_link.lock = SDL_CreateMutex();
    v.mic_up = id ? &v.audio_link : &v.video;
    v.camera_up = id ? &v.camera_link : &v.video;
    v.video.fd = dial_handshake(host, port, v.video.proto, id);
    if (v.video.fd < 0) {
        fprintf(stderr, "cast-view: cannot connect %s:%d\n", host, port);
        return 1;
    }
    if (id) {
        v.ctrl_fd = open_input_udp(host, control_port);
        if (v.ctrl_fd < 0) {
            fprintf(stderr, "cast-view: input UDP %s:%d unavailable\n", host,
                    control_port);
        }
    }

    SDL_SetHint(SDL_HINT_RENDER_SCALE_QUALITY, "linear");
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        fprintf(stderr, "cast-view: SDL: %s\n", SDL_GetError());
        return 1;
    }
    win = SDL_CreateWindow("tebox cast", SDL_WINDOWPOS_CENTERED,
                           SDL_WINDOWPOS_CENTERED, win_h * 9 / 20, win_h,
                           SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
    ren = win ? SDL_CreateRenderer(win, -1, SDL_RENDERER_ACCELERATED |
                                                SDL_RENDERER_PRESENTVSYNC)
              : NULL;
    if (!ren) {
        fprintf(stderr, "cast-view: SDL window: %s\n", SDL_GetError());
        return 1;
    }

    /* Audio is optional: without it the viewer still shows video. */
    if ((v.audio || mic) && SDL_InitSubSystem(SDL_INIT_AUDIO) < 0) {
        fprintf(stderr, "cast-view: SDL audio: %s, audio off\n", SDL_GetError());
        v.audio = false;
        mic = false;
    }
    if (mic) {
        SDL_AudioSpec want = {0};

        want.freq = CAST_AUDIO_RATE;
        want.format = AUDIO_S16LSB;
        want.channels = 1;
        want.samples = 480; /* 10 ms */
        v.mic_dev = SDL_OpenAudioDevice(NULL, 1, &want, NULL, 0);
        if (!v.mic_dev) {
            fprintf(stderr, "cast-view: microphone: %s\n", SDL_GetError());
        }
    }

    clipboard_event_type = SDL_RegisterEvents(1);
    if (v.ctrl_fd >= 0) {
        SDL_DetachThread(SDL_CreateThread(control_thread, "cast-control", &v));
        SDL_StartTextInput();
        send_ctrl(&v, "{\"type\":\"ping\"}"); /* subscribe to clipboard */
    }

    v.lock = SDL_CreateMutex();
    v.latest = av_frame_alloc();
    thread = SDL_CreateThread(video_thread, "cast-video", &v);
    if (id && (v.audio || v.mic_dev)) {
        v.audio_link.fd = dial_handshake(host, audio_port, v.audio_link.proto, id);
        audio_thr = SDL_CreateThread(audio_thread, "cast-audio", &v);
    }
    if (v.mic_dev) {
        SDL_PauseAudioDevice(v.mic_dev, 0);
        mic_thr = SDL_CreateThread(mic_thread, "cast-mic", &v);
    }
    if (v.camera) {
        avdevice_register_all();
        camera_thr = SDL_CreateThread(camera_thread, "cast-camera", &v);
    }
    fprintf(stderr, "cast-view: %s:%d%s%s input=%s audio=%s mic=%s camera=%s\n",
            host, port, id ? " id=" : "", id ? id : "",
            v.ctrl_fd >= 0 ? "udp" : "off", v.audio ? "on" : "off",
            v.mic_dev ? "on" : "off", v.camera ? v.camera : "off");

    while (!v.quit) {
        SDL_Event ev;
        float gx, gy;

        while (SDL_PollEvent(&ev)) {
            if (getenv("TEBOX_CAST_DEBUG")) {
                if (ev.type == SDL_MOUSEBUTTONDOWN || ev.type == SDL_MOUSEBUTTONUP) {
                    int cx, cy, ww, wh;

                    cursor_in_window(win, &cx, &cy);
                    SDL_GetWindowSize(win, &ww, &wh);
                    fprintf(stderr, "dbg: mouse %s btn=%d event=%d,%d cursor=%d,%d window=%dx%d\n",
                            ev.type == SDL_MOUSEBUTTONDOWN ? "down" : "up",
                            ev.button.button, ev.button.x, ev.button.y,
                            cx, cy, ww, wh);
                } else if (ev.type == SDL_FINGERDOWN || ev.type == SDL_FINGERUP) {
                    fprintf(stderr, "dbg: finger %s x=%.3f y=%.3f\n",
                            ev.type == SDL_FINGERDOWN ? "down" : "up",
                            ev.tfinger.x, ev.tfinger.y);
                } else if (ev.type == SDL_WINDOWEVENT) {
                    fprintf(stderr, "dbg: window event %d\n", ev.window.event);
                } else if (ev.type == SDL_MOUSEMOTION && (SDL_GetTicks() / 500) % 4 == 0) {
                    fprintf(stderr, "dbg: motion x=%d y=%d state=%u\n",
                            ev.motion.x, ev.motion.y, ev.motion.state);
                }
            }
            switch (ev.type) {
            case SDL_QUIT:
                v.quit = true;
                break;
            case SDL_MOUSEBUTTONDOWN:
                if (ev.button.button == SDL_BUTTON_LEFT &&
                    to_guest(&v, win, &gx, &gy)) {
                    touching = true;
                    touch_x = gx;
                    touch_y = gy;
                    touch_tick = SDL_GetTicks();
                    taps++;
                    send_touch(&v, "down", gx, gy);
                } else if (ev.button.button == SDL_BUTTON_RIGHT) {
                    send_key(&v, "back");
                }
                break;
            case SDL_MOUSEMOTION:
                if (touching) {
                    to_guest(&v, win, &gx, &gy);
                    touch_x = gx;
                    touch_y = gy;
                    touch_tick = SDL_GetTicks();
                    send_touch(&v, "move", gx, gy);
                }
                break;
            case SDL_MOUSEBUTTONUP:
                if (ev.button.button == SDL_BUTTON_LEFT && touching) {
                    touching = false;
                    to_guest(&v, win, &gx, &gy);
                    /* Twice: a lost "up" would leave the finger down until
                     * the API's 1 s watchdog releases it. */
                    send_touch(&v, "up", gx, gy);
                    send_touch(&v, "up", gx, gy);
                }
                break;
            case SDL_KEYDOWN: {
                SDL_Keycode sym = ev.key.keysym.sym;
                Uint16 mod = ev.key.keysym.mod;
                const char *nav = nav_key_for(sym);
                int code = android_keycode_for(sym);
                int meta = (mod & KMOD_SHIFT) ? ANDROID_META_SHIFT : 0;

                if (mod & (KMOD_GUI | KMOD_CTRL)) {
                    if (sym == SDLK_v) {
                        char *text = SDL_GetClipboardText();

                        if (text && text[0]) {
                            send_text_event(&v, "clipboard", text,
                                            ",\"paste\":true");
                            free(last_clip);
                            last_clip = strdup(text);
                        }
                        SDL_free(text);
                    } else if ((code = android_shortcut_for(sym))) {
                        send_keycode(&v, code, meta | ANDROID_META_CTRL);
                    }
                } else if (nav) {
                    if (!ev.key.repeat) {
                        send_key(&v, nav);
                    }
                } else if (code) {
                    send_keycode(&v, code, meta);
                }
                break;
            }
            case SDL_TEXTINPUT:
                send_text_event(&v, "text", ev.text.text, "");
                break;
            case SDL_CLIPBOARDUPDATE:
            case SDL_WINDOWEVENT:
                if (ev.type == SDL_WINDOWEVENT &&
                    ev.window.event != SDL_WINDOWEVENT_FOCUS_GAINED) {
                    break;
                }
                if (v.ctrl_fd >= 0 && SDL_HasClipboardText()) {
                    char *text = SDL_GetClipboardText();

                    if (text && text[0] &&
                        (!last_clip || strcmp(text, last_clip))) {
                        send_text_event(&v, "clipboard", text, "");
                        free(last_clip);
                        last_clip = strdup(text);
                    }
                    SDL_free(text);
                }
                break;
            default:
                if (ev.type == clipboard_event_type) {
                    char *text = ev.user.data1;

                    free(last_clip);
                    last_clip = text;
                    SDL_SetClipboardText(text);
                }
                break;
            }
        }

        /* A held finger must keep talking: the API releases a touch that has
         * been silent for 1 s (lost UDP "up"). */
        if (touching && SDL_GetTicks() - touch_tick >= 200) {
            send_touch(&v, "move", touch_x, touch_y);
            touch_tick = SDL_GetTicks();
        }
        if (v.ctrl_fd >= 0 && SDL_GetTicks() - ping_tick >= 5000) {
            send_ctrl(&v, "{\"type\":\"ping\"}"); /* keep clipboard subscription */
            ping_tick = SDL_GetTicks();
        }

        SDL_LockMutex(v.lock);
        if (v.fresh) {
            AVFrame *f = v.latest;

            if (!tex || f->width != v.frame_w || f->height != v.frame_h) {
                if (tex) {
                    SDL_DestroyTexture(tex);
                }
                tex = SDL_CreateTexture(ren, SDL_PIXELFORMAT_IYUV,
                                        SDL_TEXTUREACCESS_STREAMING,
                                        f->width, f->height);
                v.frame_w = f->width;
                v.frame_h = f->height;
            }
            if (tex && f->format == AV_PIX_FMT_YUV420P) {
                SDL_UpdateYUVTexture(tex, NULL, f->data[0], f->linesize[0],
                                     f->data[1], f->linesize[1],
                                     f->data[2], f->linesize[2]);
            }
            v.fresh = false;
        }
        SDL_UnlockMutex(v.lock);

        if (SDL_GetTicks() - title_tick >= 1000) {
            char title[128];

            snprintf(title, sizeof(title), "tebox cast %dx%d  %llu fps  taps %u%s",
                     v.frame_w, v.frame_h,
                     (unsigned long long)(v.frames - title_frames), taps,
                     v.ctrl_fd >= 0 ? "" : "  (view only)");
            SDL_SetWindowTitle(win, title);
            title_frames = v.frames;
            title_tick = SDL_GetTicks();
        }

        SDL_SetRenderDrawColor(ren, 0, 0, 0, 255);
        SDL_RenderClear(ren);
        if (tex) {
            int ww, wh, ow, oh;
            SDL_Rect r = fit_rect(win, v.frame_w, v.frame_h);

            /* fit_rect is in window points; scale to drawable pixels. */
            SDL_GetWindowSize(win, &ww, &wh);
            SDL_GetRendererOutputSize(ren, &ow, &oh);
            r.x = r.x * ow / ww;
            r.w = r.w * ow / ww;
            r.y = r.y * oh / wh;
            r.h = r.h * oh / wh;
            SDL_RenderCopy(ren, tex, NULL, &r);
        }
        SDL_RenderPresent(ren);
    }

    /* Uploaders first: they write to the links under their locks. */
    SDL_WaitThread(camera_thr, NULL);
    SDL_WaitThread(mic_thr, NULL);
    if (v.mic_dev) {
        SDL_CloseAudioDevice(v.mic_dev);
    }
    if (v.video.fd >= 0) {
        shutdown(v.video.fd, SHUT_RDWR);
    }
    if (v.audio_link.fd >= 0) {
        shutdown(v.audio_link.fd, SHUT_RDWR);
    }
    SDL_WaitThread(thread, NULL);
    SDL_WaitThread(audio_thr, NULL);
    set_link_fd(&v.video, -1);
    set_link_fd(&v.audio_link, -1);
    set_link_fd(&v.camera_link, -1);
    free(last_clip);
    if (tex) {
        SDL_DestroyTexture(tex);
    }
    av_frame_free(&v.latest);
    SDL_DestroyMutex(v.lock);
    SDL_DestroyMutex(v.video.lock);
    SDL_DestroyMutex(v.audio_link.lock);
    SDL_DestroyMutex(v.camera_link.lock);
    SDL_DestroyRenderer(ren);
    SDL_DestroyWindow(win);
    SDL_Quit();
    return 0;
}
