#pragma once

#include <android/hardware_buffer.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gki {

/* BT.601 limited-range RGB(A) → NV12. |rowBytes| is the source stride in bytes. */
std::vector<uint8_t> rgbToNv12(const uint8_t* pixels, uint32_t width, uint32_t height,
                               size_t rowBytes, unsigned channels);

/* Host-decoded RGBA → YV12 for ByteBuffer clients (CCodec cannot convert RGBA). */
AHardwareBuffer* rgbaToYv12(AHardwareBuffer* rgba, uint32_t width, uint32_t height);

}  // namespace gki
