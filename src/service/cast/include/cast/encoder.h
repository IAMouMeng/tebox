#ifndef TEBOX_CAST_ENCODER_H
#define TEBOX_CAST_ENCODER_H

#include "cast/protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cast_encoder;

struct cast_encoder_cfg {
    enum cast_codec codec;
    int width;          /* 0 = use source */
    int height;
    int max_width;      /* 0 = unlimited */
    int max_height;
    int fps;
    int64_t bitrate;    /* bits/sec */
    int gop_sec;        /* keyint seconds; default 2 */
    bool prefer_hw;     /* VideoToolbox / VAAPI when available */
    const char *force_encoder; /* optional FFmpeg encoder name */
};

struct cast_packet {
    enum cast_codec codec;
    uint8_t flags;
    uint64_t pts_us;
    uint8_t *data;
    size_t size;
};

struct cast_encoder *cast_encoder_create(const struct cast_encoder_cfg *cfg);
void cast_encoder_destroy(struct cast_encoder *enc);

/* BGRA8888 top-down input. Returns 0 on success (packets may be 0). */
int cast_encoder_push_bgra(struct cast_encoder *enc,
                           const uint8_t *bgra, int width, int height,
                           int stride, uint64_t pts_us,
                           struct cast_packet **out, int *out_count);

void cast_encoder_free_packets(struct cast_packet *pkts, int count);

int cast_encoder_force_idr(struct cast_encoder *enc);
int cast_encoder_reconfigure(struct cast_encoder *enc,
                             const struct cast_encoder_cfg *cfg);

const char *cast_encoder_name(const struct cast_encoder *enc);
void cast_encoder_get_cfg(const struct cast_encoder *enc,
                          struct cast_encoder_cfg *out);

#ifdef __cplusplus
}
#endif

#endif
