#include "ColorConvert.h"

#include <algorithm>
#include <vector>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif

namespace gki {

std::vector<uint8_t> rgbToNv12(const uint8_t* pixels, uint32_t width, uint32_t height,
                               size_t rowBytes, unsigned channels) {
    if (!pixels || !width || !height || channels < 3 ||
        rowBytes < static_cast<size_t>(width) * channels)
        return {};
    const size_t ySize = static_cast<size_t>(width) * height;
    std::vector<uint8_t> nv12(ySize + ySize / 2, 128);
    auto clampByte = [](int value) -> uint8_t {
        return static_cast<uint8_t>(std::max(0, std::min(255, value)));
    };
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = pixels + static_cast<size_t>(y) * rowBytes;
        uint8_t* yout = nv12.data() + static_cast<size_t>(y) * width;
        uint32_t x = 0;
#if defined(__aarch64__)
        if (channels == 4) {
            for (; x + 8 <= width; x += 8) {
                uint8x8x4_t rgba = vld4_u8(row + static_cast<size_t>(x) * 4);
                uint16x8_t yv = vmull_u8(rgba.val[0], vdup_n_u8(66));
                yv = vmlal_u8(yv, rgba.val[1], vdup_n_u8(129));
                yv = vmlal_u8(yv, rgba.val[2], vdup_n_u8(25));
                yv = vaddq_u16(vshrq_n_u16(vaddq_u16(yv, vdupq_n_u16(128)), 8),
                               vdupq_n_u16(16));
                vst1_u8(yout + x, vmovn_u16(yv));
            }
        }
#endif
        for (; x < width; ++x) {
            const uint8_t* p = row + static_cast<size_t>(x) * channels;
            const int r = p[0], g = p[1], b = p[2];
            yout[x] = clampByte(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    uint8_t* uv = nv12.data() + ySize;
    for (uint32_t y = 0; y < height; y += 2) {
        for (uint32_t x = 0; x < width; x += 2) {
            int u = 0, v = 0, count = 0;
            for (uint32_t dy = 0; dy < 2 && y + dy < height; ++dy) {
                for (uint32_t dx = 0; dx < 2 && x + dx < width; ++dx) {
                    const uint8_t* p = pixels + static_cast<size_t>(y + dy) * rowBytes +
                                       (x + dx) * channels;
                    const int r = p[0], g = p[1], b = p[2];
                    u += ((-38 * r - 74 * g + 112 * b + 128) >> 8) + 128;
                    v += ((112 * r - 94 * g - 18 * b + 128) >> 8) + 128;
                    ++count;
                }
            }
            const size_t uvOffset = static_cast<size_t>(y / 2) * width + x;
            uv[uvOffset] = clampByte(u / count);
            if (x + 1 < width) uv[uvOffset + 1] = clampByte(v / count);
        }
    }
    return nv12;
}

AHardwareBuffer* rgbaToYv12(AHardwareBuffer* rgba, uint32_t width, uint32_t height) {
    constexpr uint32_t kFormatYv12 = 0x32315659;  // HAL_PIXEL_FORMAT_YV12
    AHardwareBuffer_Desc desc = {};
    desc.width = width;
    desc.height = height;
    desc.layers = 1;
    desc.format = kFormatYv12;
    desc.usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN;
    AHardwareBuffer* yuv = nullptr;
    if (AHardwareBuffer_allocate(&desc, &yuv) != 0) return nullptr;
    AHardwareBuffer_Desc srcDesc;
    AHardwareBuffer_describe(rgba, &srcDesc);
    void* src = nullptr;
    AHardwareBuffer_Planes planes;
    if (AHardwareBuffer_lock(rgba, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &src) != 0) {
        AHardwareBuffer_release(yuv);
        return nullptr;
    }
    if (AHardwareBuffer_lockPlanes(yuv, AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN, -1, nullptr,
                                   &planes) != 0 ||
        planes.planeCount < 3) {
        AHardwareBuffer_unlock(rgba, nullptr);
        AHardwareBuffer_release(yuv);
        return nullptr;
    }
    const uint8_t* in = static_cast<const uint8_t*>(src);
    const size_t inStride = static_cast<size_t>(srcDesc.stride) * 4;
    const AHardwareBuffer_Plane& py = planes.planes[0];
    const AHardwareBuffer_Plane& pu = planes.planes[1];
    const AHardwareBuffer_Plane& pv = planes.planes[2];
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* row = in + y * inStride;
        uint8_t* out = static_cast<uint8_t*>(py.data) + y * py.rowStride;
        for (uint32_t x = 0; x < width; ++x) {
            const int r = row[4 * x], g = row[4 * x + 1], b = row[4 * x + 2];
            out[x] = static_cast<uint8_t>(((66 * r + 129 * g + 25 * b + 128) >> 8) + 16);
        }
    }
    for (uint32_t y = 0; y + 1 < height; y += 2) {
        const uint8_t* r0 = in + y * inStride;
        const uint8_t* r1 = r0 + inStride;
        uint8_t* u = static_cast<uint8_t*>(pu.data) + (y / 2) * pu.rowStride;
        uint8_t* v = static_cast<uint8_t*>(pv.data) + (y / 2) * pv.rowStride;
        for (uint32_t x = 0; x + 1 < width; x += 2) {
            const int r = r0[4 * x] + r0[4 * x + 4] + r1[4 * x] + r1[4 * x + 4];
            const int g = r0[4 * x + 1] + r0[4 * x + 5] + r1[4 * x + 1] + r1[4 * x + 5];
            const int b = r0[4 * x + 2] + r0[4 * x + 6] + r1[4 * x + 2] + r1[4 * x + 6];
            u[(x / 2) * pu.pixelStride] =
                    static_cast<uint8_t>(((-38 * r - 74 * g + 112 * b + 512) >> 10) + 128);
            v[(x / 2) * pv.pixelStride] =
                    static_cast<uint8_t>(((112 * r - 94 * g - 18 * b + 512) >> 10) + 128);
        }
    }
    AHardwareBuffer_unlock(yuv, nullptr);
    AHardwareBuffer_unlock(rgba, nullptr);
    return yuv;
}

}  // namespace gki
