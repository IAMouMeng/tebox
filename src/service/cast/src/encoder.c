#include "cast/encoder.h"

#include <libavcodec/avcodec.h>
#include <libavutil/imgutils.h>
#include <libavutil/opt.h>
#include <libswscale/swscale.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct cast_encoder {
    struct cast_encoder_cfg cfg;
    const AVCodec *codec;
    AVCodecContext *ctx;
    AVFrame *frame;
    AVPacket *pkt;
    struct SwsContext *sws;
    int src_w, src_h, src_stride;
    int enc_w, enc_h;
    int64_t next_pts;
    bool force_idr;
    char encoder_name[64];
};

static void scale_dims(int src_w, int src_h, int max_w, int max_h,
                       int *out_w, int *out_h)
{
    int w = src_w;
    int h = src_h;

    if (max_w > 0 && w > max_w) {
        h = (int)((int64_t)h * max_w / w);
        w = max_w;
    }
    if (max_h > 0 && h > max_h) {
        w = (int)((int64_t)w * max_h / h);
        h = max_h;
    }
    /* Even dimensions for most codecs. */
    w &= ~1;
    h &= ~1;
    if (w < 2) {
        w = 2;
    }
    if (h < 2) {
        h = 2;
    }
    *out_w = w;
    *out_h = h;
}

static const char *pick_encoder(enum cast_codec codec, bool prefer_hw,
                                const char *force)
{
    if (force && force[0]) {
        return force;
    }
#if defined(__APPLE__)
    if (prefer_hw) {
        if (codec == CAST_CODEC_H264) {
            return "h264_videotoolbox";
        }
        if (codec == CAST_CODEC_HEVC) {
            return "hevc_videotoolbox";
        }
    }
#else
    if (prefer_hw) {
        if (codec == CAST_CODEC_H264) {
            return "h264_vaapi";
        }
        if (codec == CAST_CODEC_HEVC) {
            return "hevc_vaapi";
        }
        if (codec == CAST_CODEC_VP9) {
            return "vp9_vaapi";
        }
        if (codec == CAST_CODEC_AV1) {
            return "av1_vaapi";
        }
    }
#endif
    switch (codec) {
    case CAST_CODEC_H264: return "libx264";
    case CAST_CODEC_HEVC: return "libx265";
    case CAST_CODEC_VP8:  return "libvpx";
    case CAST_CODEC_VP9:  return "libvpx-vp9";
    case CAST_CODEC_AV1:  return "libsvtav1";
    default: return "libx264";
    }
}

static const char *fallback_encoder(enum cast_codec codec)
{
    switch (codec) {
    case CAST_CODEC_H264: return "libx264";
    case CAST_CODEC_HEVC: return "libx265";
    case CAST_CODEC_VP8:  return "libvpx";
    case CAST_CODEC_VP9:  return "libvpx-vp9";
    case CAST_CODEC_AV1:  return "libaom-av1";
    default: return "libx264";
    }
}

static int open_codec(struct cast_encoder *enc)
{
    const char *name = pick_encoder(enc->cfg.codec, enc->cfg.prefer_hw,
                                    enc->cfg.force_encoder);
    const AVCodec *codec = avcodec_find_encoder_by_name(name);
    int gop;
    int fps = enc->cfg.fps > 0 ? enc->cfg.fps : 30;
    int gop_sec = enc->cfg.gop_sec > 0 ? enc->cfg.gop_sec : 2;

    if (!codec) {
        name = fallback_encoder(enc->cfg.codec);
        codec = avcodec_find_encoder_by_name(name);
    }
    if (!codec && enc->cfg.codec == CAST_CODEC_AV1) {
        name = "libaom-av1";
        codec = avcodec_find_encoder_by_name(name);
    }
    if (!codec) {
        fprintf(stderr, "cast-encoder: no encoder for %s\n",
                cast_codec_name(enc->cfg.codec));
        return -1;
    }

    enc->codec = codec;
    snprintf(enc->encoder_name, sizeof(enc->encoder_name), "%s", name);

    enc->ctx = avcodec_alloc_context3(codec);
    if (!enc->ctx) {
        return -1;
    }

    enc->ctx->width = enc->enc_w;
    enc->ctx->height = enc->enc_h;
    enc->ctx->time_base = (AVRational){1, fps};
    enc->ctx->framerate = (AVRational){fps, 1};
    enc->ctx->bit_rate = enc->cfg.bitrate > 0 ? enc->cfg.bitrate : 4000000;
    enc->ctx->gop_size = gop = fps * gop_sec;
    enc->ctx->max_b_frames = 0;
    enc->ctx->delay = 0;
    enc->ctx->flags |= AV_CODEC_FLAG_LOW_DELAY;
    enc->ctx->pix_fmt = AV_PIX_FMT_YUV420P;

    if (strstr(name, "videotoolbox")) {
        enc->ctx->pix_fmt = AV_PIX_FMT_NV12;
        av_opt_set_int(enc->ctx->priv_data, "realtime", 1, 0);
        av_opt_set_int(enc->ctx->priv_data, "prio_speed", 1, 0);
        av_opt_set(enc->ctx->priv_data, "profile", "baseline", 0);
    } else if (!strcmp(name, "libx264") || !strcmp(name, "libx265")) {
        av_opt_set(enc->ctx->priv_data, "preset", "ultrafast", 0);
        av_opt_set(enc->ctx->priv_data, "tune", "zerolatency", 0);
    } else if (strstr(name, "libvpx")) {
        av_opt_set(enc->ctx->priv_data, "deadline", "realtime", 0);
        av_opt_set_int(enc->ctx->priv_data, "cpu-used", 8, 0);
    } else if (strstr(name, "svtav1") || strstr(name, "aom")) {
        av_opt_set(enc->ctx->priv_data, "preset", "10", 0);
    }

    if (avcodec_open2(enc->ctx, codec, NULL) < 0) {
        fprintf(stderr, "cast-encoder: open %s failed, trying soft fallback\n",
                name);
        avcodec_free_context(&enc->ctx);
        name = fallback_encoder(enc->cfg.codec);
        codec = avcodec_find_encoder_by_name(name);
        if (!codec) {
            return -1;
        }
        enc->codec = codec;
        snprintf(enc->encoder_name, sizeof(enc->encoder_name), "%s", name);
        enc->ctx = avcodec_alloc_context3(codec);
        if (!enc->ctx) {
            return -1;
        }
        enc->ctx->width = enc->enc_w;
        enc->ctx->height = enc->enc_h;
        enc->ctx->time_base = (AVRational){1, fps};
        enc->ctx->framerate = (AVRational){fps, 1};
        enc->ctx->bit_rate = enc->cfg.bitrate > 0 ? enc->cfg.bitrate : 4000000;
        enc->ctx->gop_size = gop;
        enc->ctx->max_b_frames = 0;
        enc->ctx->pix_fmt = AV_PIX_FMT_YUV420P;
        if (!strcmp(name, "libx264") || !strcmp(name, "libx265")) {
            av_opt_set(enc->ctx->priv_data, "preset", "veryfast", 0);
            av_opt_set(enc->ctx->priv_data, "tune", "zerolatency", 0);
        }
        if (avcodec_open2(enc->ctx, codec, NULL) < 0) {
            avcodec_free_context(&enc->ctx);
            return -1;
        }
    }

    enc->frame = av_frame_alloc();
    enc->pkt = av_packet_alloc();
    if (!enc->frame || !enc->pkt) {
        return -1;
    }
    enc->frame->format = enc->ctx->pix_fmt;
    enc->frame->width = enc->enc_w;
    enc->frame->height = enc->enc_h;
    if (av_frame_get_buffer(enc->frame, 32) < 0) {
        return -1;
    }

    fprintf(stderr, "cast-encoder: %s %dx%d @%dfps bitrate=%lld gop=%d\n",
            enc->encoder_name, enc->enc_w, enc->enc_h, fps,
            (long long)enc->ctx->bit_rate, gop);
    return 0;
}

static void close_codec(struct cast_encoder *enc)
{
    if (enc->ctx) {
        avcodec_send_frame(enc->ctx, NULL);
        if (enc->pkt) {
            while (avcodec_receive_packet(enc->ctx, enc->pkt) >= 0) {
                av_packet_unref(enc->pkt);
            }
        }
    }
    if (enc->sws) {
        sws_freeContext(enc->sws);
        enc->sws = NULL;
    }
    av_packet_free(&enc->pkt);
    av_frame_free(&enc->frame);
    avcodec_free_context(&enc->ctx);
    enc->codec = NULL;
}

struct cast_encoder *cast_encoder_create(const struct cast_encoder_cfg *cfg)
{
    struct cast_encoder *enc = calloc(1, sizeof(*enc));

    if (!enc) {
        return NULL;
    }
    enc->cfg = *cfg;
    if (enc->cfg.fps <= 0) {
        enc->cfg.fps = 30;
    }
    if (enc->cfg.gop_sec <= 0) {
        enc->cfg.gop_sec = 1;
    }
    if (enc->cfg.bitrate <= 0) {
        enc->cfg.bitrate = 4000000;
    }
    /* Defer open_codec until the first real frame so capture can set size. */
    if (cfg->width > 0 && cfg->height > 0) {
        enc->enc_w = cfg->width;
        enc->enc_h = cfg->height;
        scale_dims(enc->enc_w, enc->enc_h, enc->cfg.max_width,
                   enc->cfg.max_height, &enc->enc_w, &enc->enc_h);
        if (open_codec(enc) < 0) {
            free(enc);
            return NULL;
        }
    } else {
        snprintf(enc->encoder_name, sizeof(enc->encoder_name), "%s",
                 pick_encoder(enc->cfg.codec, enc->cfg.prefer_hw,
                              enc->cfg.force_encoder));
    }
    return enc;
}

void cast_encoder_destroy(struct cast_encoder *enc)
{
    if (!enc) {
        return;
    }
    close_codec(enc);
    free(enc);
}

const char *cast_encoder_name(const struct cast_encoder *enc)
{
    return enc ? enc->encoder_name : "";
}

void cast_encoder_get_cfg(const struct cast_encoder *enc,
                          struct cast_encoder_cfg *out)
{
    if (enc && out) {
        *out = enc->cfg;
    }
}

int cast_encoder_force_idr(struct cast_encoder *enc)
{
    if (!enc) {
        return -1;
    }
    enc->force_idr = true;
    return 0;
}

int cast_encoder_reconfigure(struct cast_encoder *enc,
                             const struct cast_encoder_cfg *cfg)
{
    if (!enc || !cfg) {
        return -1;
    }
    close_codec(enc);
    enc->cfg = *cfg;
    if (enc->cfg.fps <= 0) {
        enc->cfg.fps = 30;
    }
    if (enc->cfg.gop_sec <= 0) {
        enc->cfg.gop_sec = 1;
    }
    if (enc->cfg.bitrate <= 0) {
        enc->cfg.bitrate = 4000000;
    }
    if (enc->src_w > 0 && enc->src_h > 0) {
        scale_dims(enc->src_w, enc->src_h, enc->cfg.max_width,
                   enc->cfg.max_height, &enc->enc_w, &enc->enc_h);
    } else {
        enc->enc_w = cfg->width > 0 ? cfg->width : enc->enc_w;
        enc->enc_h = cfg->height > 0 ? cfg->height : enc->enc_h;
        scale_dims(enc->enc_w, enc->enc_h, enc->cfg.max_width,
                   enc->cfg.max_height, &enc->enc_w, &enc->enc_h);
    }
    enc->next_pts = 0;
    enc->force_idr = true;
    return open_codec(enc);
}

void cast_encoder_free_packets(struct cast_packet *pkts, int count)
{
    int i;

    if (!pkts) {
        return;
    }
    for (i = 0; i < count; i++) {
        free(pkts[i].data);
    }
    free(pkts);
}

static int collect_packets(struct cast_encoder *enc,
                           struct cast_packet **out, int *out_count)
{
    struct cast_packet *pkts = NULL;
    int n = 0;
    int ret;

    while ((ret = avcodec_receive_packet(enc->ctx, enc->pkt)) >= 0) {
        struct cast_packet *np;
        uint8_t *buf;

        np = realloc(pkts, (size_t)(n + 1) * sizeof(*np));
        if (!np) {
            cast_encoder_free_packets(pkts, n);
            av_packet_unref(enc->pkt);
            return -1;
        }
        pkts = np;
        buf = malloc((size_t)enc->pkt->size);
        if (!buf) {
            cast_encoder_free_packets(pkts, n);
            av_packet_unref(enc->pkt);
            return -1;
        }
        memcpy(buf, enc->pkt->data, (size_t)enc->pkt->size);
        pkts[n].codec = enc->cfg.codec;
        pkts[n].flags = (enc->pkt->flags & AV_PKT_FLAG_KEY) ? CAST_FLAG_KEY : 0;
        pkts[n].pts_us = enc->pkt->pts >= 0
            ? (uint64_t)enc->pkt->pts * 1000000ull /
              (uint64_t)enc->cfg.fps
            : 0;
        pkts[n].data = buf;
        pkts[n].size = (size_t)enc->pkt->size;
        n++;
        av_packet_unref(enc->pkt);
    }
    *out = pkts;
    *out_count = n;
    return (ret == AVERROR(EAGAIN) || ret == AVERROR_EOF) ? 0 : ret;
}

int cast_encoder_push_bgra(struct cast_encoder *enc,
                           const uint8_t *bgra, int width, int height,
                           int stride, uint64_t pts_us,
                           struct cast_packet **out, int *out_count)
{
    const uint8_t *src_slices[4] = {bgra, NULL, NULL, NULL};
    int src_stride[4] = {stride, 0, 0, 0};
    int tw, th;
    int ret;

    *out = NULL;
    *out_count = 0;
    if (!enc || !bgra || width <= 0 || height <= 0) {
        return -1;
    }

    scale_dims(width, height, enc->cfg.max_width, enc->cfg.max_height,
               &tw, &th);
    if (!enc->ctx) {
        struct cast_encoder_cfg cfg = enc->cfg;

        cfg.width = tw;
        cfg.height = th;
        enc->enc_w = tw;
        enc->enc_h = th;
        if (open_codec(enc) < 0) {
            return -1;
        }
    } else if (tw != enc->enc_w || th != enc->enc_h) {
        struct cast_encoder_cfg cfg = enc->cfg;

        cfg.width = tw;
        cfg.height = th;
        if (cast_encoder_reconfigure(enc, &cfg) < 0) {
            return -1;
        }
    }

    if (!enc->sws || enc->src_w != width || enc->src_h != height ||
        enc->src_stride != stride) {
        if (enc->sws) {
            sws_freeContext(enc->sws);
        }
        enc->sws = sws_getContext(width, height, AV_PIX_FMT_BGRA,
                                  enc->enc_w, enc->enc_h, enc->ctx->pix_fmt,
                                  SWS_BILINEAR, NULL, NULL, NULL);
        enc->src_w = width;
        enc->src_h = height;
        enc->src_stride = stride;
        if (!enc->sws) {
            return -1;
        }
    }

    if (av_frame_make_writable(enc->frame) < 0) {
        return -1;
    }
    sws_scale(enc->sws, src_slices, src_stride, 0, height,
              enc->frame->data, enc->frame->linesize);

    if (pts_us) {
        enc->frame->pts = (int64_t)(pts_us * (uint64_t)enc->cfg.fps / 1000000ull);
    } else {
        enc->frame->pts = enc->next_pts++;
    }
    if (enc->force_idr) {
        enc->frame->pict_type = AV_PICTURE_TYPE_I;
        enc->frame->flags |= AV_FRAME_FLAG_KEY;
        enc->force_idr = false;
    } else {
        enc->frame->pict_type = AV_PICTURE_TYPE_NONE;
        enc->frame->flags &= ~AV_FRAME_FLAG_KEY;
    }

    ret = avcodec_send_frame(enc->ctx, enc->frame);
    if (ret < 0) {
        return ret;
    }
    return collect_packets(enc, out, out_count);
}
