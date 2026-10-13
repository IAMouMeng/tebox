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
#define CAST_FMT_RGBA8888 1u /* GLES readback (egl-headless) */

enum cast_codec {
    CAST_CODEC_H264 = 0,
    CAST_CODEC_HEVC = 1,
    CAST_CODEC_VP8  = 2,
    CAST_CODEC_VP9  = 3,
    CAST_CODEC_AV1  = 4,
    /* Server -> viewer audio: AAC-LC 48 kHz stereo, one ADTS frame per record. */
    CAST_CODEC_AAC  = 16,
    /* Viewer -> server (same record format, sent after the handshake line):
     * a camera frame as one JPEG (any size, the server scales it), and
     * microphone audio as PCM s16le 48 kHz mono. */
    CAST_CODEC_MJPEG = 32,
    CAST_CODEC_PCM_S16LE = 33,
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
/* Largest record a viewer may send upstream (one camera JPEG). */
#define CAST_UPLINK_MAX (4u << 20)

/* Guest media over virtio-serial (QEMU chardev sockets next to cast.sock):
 *   audio-out.sock  guest -> host  PCM s16le 48 kHz stereo
 *   audio-in.sock   host -> guest  PCM s16le 48 kHz mono (microphone)
 *   camera.sock     host -> guest  camera frames, each a cast_camera_header
 *                                  followed by NV12 (width*height*3/2 bytes)
 * Multi-byte header fields are little-endian (both ends are ARM64). */
#define CAST_AUDIO_RATE       48000
#define CAST_CAMERA_MAGIC     0x4d414354u /* 'TCAM' */
#define CAST_CAMERA_FMT_NV12  1u
#define CAST_CAMERA_WIDTH     1280
#define CAST_CAMERA_HEIGHT    720

struct cast_camera_header {
    uint32_t magic;
    uint32_t format;
    uint32_t width;
    uint32_t height;
    uint32_t size;
    uint32_t reserved;
    uint64_t pts_us;
} __attribute__((packed));

const char *cast_codec_name(enum cast_codec c);
enum cast_codec cast_codec_from_name(const char *name);

#ifdef __cplusplus
}
#endif

#endif
