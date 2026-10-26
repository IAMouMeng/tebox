#pragma once

#include <cstdint>

namespace gki {

/* Guest↔host CAST_VDEC wire (mirrors cast/protocol.h). */
constexpr uint32_t kVdecMagic = 0x43454456u;  // 'VDEC'
enum : uint32_t {
    kVdecConfig = 1,
    kVdecInput = 2,
    kVdecOutput = 3,
    kVdecFlush = 4,
    kVdecClose = 5,
    kVdecPicture = 16,
    kVdecFlushed = 17,
};
constexpr uint32_t kVdecFlagEos = 1;
constexpr uint32_t kVdecFlagConfig = 2;

struct __attribute__((packed)) VdecHdr {
    uint32_t magic, type, size;
};
struct __attribute__((packed)) VdecConfig {
    uint32_t session, codec, width, height;
};
struct __attribute__((packed)) VdecInput {
    uint32_t session;
    uint64_t index;
    int64_t pts_us;
    uint32_t flags;
};
struct __attribute__((packed)) VdecOutput {
    uint32_t session;
    uint64_t id;
    uint32_t resource_id, width, height;
};
struct __attribute__((packed)) VdecPicture {
    uint32_t session;
    uint64_t id, index;
    int64_t pts_us;
    int32_t status;
    uint32_t flags, width, height;
};

}  // namespace gki
