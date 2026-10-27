#pragma once

#include <cstdint>

namespace gki {

/* Guest↔host CAST_VENC wire (mirrors cast/protocol.h). */
constexpr uint32_t kVencMagic = 0x434e4556u;  // 'VENC'
enum : uint32_t {
    kVencConfig = 1,
    kVencFrame = 2,
    kVencClose = 3,
    kVencBitstream = 16,
};
constexpr uint32_t kVencFlagKey = 1;

struct __attribute__((packed)) VencHdr {
    uint32_t magic, type, size;
};
struct __attribute__((packed)) VencConfig {
    uint32_t session, width, height, bitrate, fps, gop_sec;
    uint32_t codec; /* CAST_CODEC_*: 0=H264, 1=HEVC, 2=VP8, 3=VP9 */
};
enum : uint32_t {
    kCastCodecH264 = 0,
    kCastCodecHevc = 1,
    kCastCodecVp8 = 2,
    kCastCodecVp9 = 3,
};
struct __attribute__((packed)) VencFrame {
    uint32_t session;
    uint64_t index;
    int64_t pts_us;
    uint32_t resource_id, width, height, format, flags;
};
struct __attribute__((packed)) VencOut {
    uint32_t session;
    uint64_t index;
    int64_t pts_us;
    int32_t status;
    uint32_t flags;
};

}  // namespace gki
