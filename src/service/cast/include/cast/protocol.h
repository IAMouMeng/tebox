/* tebox host cast wire formats (raw frames + encoded stream). */
#ifndef TEBOX_CAST_PROTOCOL_H
#define TEBOX_CAST_PROTOCOL_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define CAST_RAW_MAGIC   0x54434652u /* 'TCFR' */
#define CAST_RAW_VERSION 1u

#define CAST_FMT_BGRA8888 0u

enum cast_codec {
    CAST_CODEC_H264 = 0,
    CAST_CODEC_HEVC = 1,
    CAST_CODEC_VP8  = 2,
    CAST_CODEC_VP9  = 3,
    CAST_CODEC_AV1  = 4,
};

#define CAST_FLAG_KEY 0x01u

/* QEMU → tebox-cast raw frame header (all multi-byte fields big-endian). */
struct cast_raw_header {
    uint32_t magic;
    uint32_t version;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t pts_us;
    uint32_t nbytes;
} __attribute__((packed));

/* Encoded stream: [u32 be length][u8 codec][u8 flags][u64 be pts_us][payload]
 * length covers codec..end of payload (not including the length field). */
#define CAST_ENC_HDR_SIZE 10u

const char *cast_codec_name(enum cast_codec c);
enum cast_codec cast_codec_from_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif
