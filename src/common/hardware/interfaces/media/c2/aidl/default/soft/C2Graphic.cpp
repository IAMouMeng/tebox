#include "C2Graphic.h"

#include "ColorConvert.h"
#include "buffer_handle.h"

#include <android/hardware_buffer_aidl.h>
#include <android/hardware/graphics/mapper/IMapper.h>
#include <android/log.h>
#include <cutils/native_handle.h>
#include <dlfcn.h>
#include <unistd.h>

#include <atomic>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "c2-graphic"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)

using aidl::android::hardware::HardwareBuffer;
using aidl::android::hardware::common::NativeHandle;

namespace gki {

std::vector<uint8_t> readHardwareBuffer(const HardwareBuffer& hw) {
    AHardwareBuffer* buffer = hw.get();
    if (!buffer) return {};
    AHardwareBuffer_Desc desc = {};
    AHardwareBuffer_describe(buffer, &desc);
    if (!desc.width || !desc.height || desc.layers != 1) return {};
    if (desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM ||
        desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM ||
        desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM) {
        void* data = nullptr;
        if (AHardwareBuffer_lock(buffer, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, -1, nullptr, &data) !=
                    0 ||
            !data)
            return {};
        const unsigned channels = desc.format == AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM ? 3 : 4;
        const size_t rowBytes = static_cast<size_t>(desc.stride) * channels;
        std::vector<uint8_t> out =
                rgbToNv12(static_cast<const uint8_t*>(data), desc.width, desc.height, rowBytes,
                          channels);
        int fence = -1;
        AHardwareBuffer_unlock(buffer, &fence);
        if (fence >= 0) close(fence);
        return out;
    }
    return {};
}

std::vector<uint8_t> readNativeGraphic(const NativeHandle& handle) {
    // C2AllocatorGralloc wraps the allocator's native handle by appending
    // eleven metadata ints (width, height, format, usage, stride, ... magic).
    // The AIDL NativeHandle delivered by the HAL contains that complete
    // wrapped handle, so strip the metadata before handing it to mapper.
    constexpr size_t kC2ExtraInts = 11;
    // C2's multicharacter literal '\\xc2gr\\0' is laid out as 0xc2677200
    // by the Android clang toolchain (the value used by C2HandleGralloc).
    constexpr int32_t kC2Magic = static_cast<int32_t>(0xc2677200u);
    if (handle.fds.empty() || handle.ints.size() < 6) return {};

    size_t baseInts = handle.ints.size();
    const int32_t* metadata = nullptr;
    if (handle.ints.size() >= kC2ExtraInts) {
        const size_t extra = handle.ints.size() - kC2ExtraInts;
        if (handle.ints[extra + 10] == kC2Magic) {
            baseInts = extra;
            metadata = handle.ints.data() + extra;
        }
    }

    static std::atomic<int> graphicDiagnostics{0};
    if (graphicDiagnostics.fetch_add(1) < 8) {
        ALOGI("graphic handle fds=%zu ints=%zu base=%zu wrapped=%d first=%d,%d,%d,%d,%d,%d "
              "last=%d,%d,%d,%d,%d",
              handle.fds.size(), handle.ints.size(), baseInts, metadata ? 1 : 0, handle.ints[0],
              handle.ints[1], handle.ints[2], handle.ints[3], handle.ints[4], handle.ints[5],
              handle.ints[handle.ints.size() - 5], handle.ints[handle.ints.size() - 4],
              handle.ints[handle.ints.size() - 3], handle.ints[handle.ints.size() - 2],
              handle.ints[handle.ints.size() - 1]);
    }

    if (baseInts < 6) return {};
    const int width = metadata ? metadata[0] : handle.ints[0];
    const int height = metadata ? metadata[1] : handle.ints[1];
    const int format = metadata ? metadata[2] : handle.ints[3];
    const int stride = metadata ? metadata[5] : handle.ints[2];
    if (width <= 0 || height <= 0 || stride < width) return {};
    const unsigned channels = format == AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM ? 3 : 4;
    if (format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM &&
        format != AHARDWAREBUFFER_FORMAT_R8G8B8X8_UNORM &&
        format != AHARDWAREBUFFER_FORMAT_R8G8B8_UNORM)
        return {};

    native_handle_t* raw =
            native_handle_create(static_cast<int>(handle.fds.size()), static_cast<int>(baseInts));
    if (!raw) return {};
    for (size_t i = 0; i < handle.fds.size(); ++i) raw->data[i] = dup(handle.fds[i].get());
    for (size_t i = 0; i < baseInts; ++i) raw->data[raw->numFds + i] = handle.ints[i];

    void* mapperLib = dlopen("/vendor/lib64/hw/mapper.stub.so", RTLD_NOW | RTLD_LOCAL);
    using LoadMapper = AIMapper_Error (*)(AIMapper**);
    auto loadMapper =
            mapperLib ? reinterpret_cast<LoadMapper>(dlsym(mapperLib, "AIMapper_loadIMapper"))
                      : nullptr;
    AIMapper* mapper = nullptr;
    std::vector<uint8_t> out;
    if (loadMapper && loadMapper(&mapper) == AIMAPPER_ERROR_NONE && mapper &&
        mapper->version == AIMAPPER_VERSION_5) {
        buffer_handle_t imported = nullptr;
        const AIMapper_Error importStatus = mapper->v5.importBuffer(raw, &imported);
        if (graphicDiagnostics.load() <= 8) {
            ALOGI("graphic mapper import status=%d imported=%p size=%d/%d/%d", importStatus,
                  imported, width, height, stride);
        }
        if (importStatus == AIMAPPER_ERROR_NONE && imported) {
            void* data = nullptr;
            ARect region{0, 0, width, height};
            const AIMapper_Error lockStatus = mapper->v5.lock(
                    imported, AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN, region, -1, &data);
            if (graphicDiagnostics.load() <= 8) {
                ALOGI("graphic mapper lock status=%d data=%p", lockStatus, data);
            }
            if (lockStatus == AIMAPPER_ERROR_NONE && data) {
                const uint8_t* sample = static_cast<const uint8_t*>(data);
                if (graphicDiagnostics.load() <= 8) {
                    ALOGI("graphic pixels first=%02x%02x%02x%02x center=%02x%02x%02x%02x",
                          sample[0], sample[1], sample[2], sample[3],
                          sample[(static_cast<size_t>(height / 2) * stride + width / 2) * channels],
                          sample[(static_cast<size_t>(height / 2) * stride + width / 2) * channels +
                                 1],
                          sample[(static_cast<size_t>(height / 2) * stride + width / 2) * channels +
                                 2],
                          sample[(static_cast<size_t>(height / 2) * stride + width / 2) * channels +
                                 3]);
                }
                out = rgbToNv12(static_cast<const uint8_t*>(data), width, height,
                                static_cast<size_t>(stride) * channels, channels);
                int fence = -1;
                mapper->v5.unlock(imported, &fence);
                if (fence >= 0) close(fence);
            }
            mapper->v5.freeBuffer(imported);
        }
    }
    native_handle_close(raw);
    native_handle_delete(raw);
    if (mapperLib) dlclose(mapperLib);
    return out;
}

bool getNativeGraphicInfo(const NativeHandle& handle, NativeGraphicInfo* out) {
    constexpr size_t kC2ExtraInts = 11;
    constexpr int32_t kC2Magic = static_cast<int32_t>(0xc2677200u);
    if (!out || handle.fds.empty() || handle.ints.size() < 6) return false;
    size_t baseInts = handle.ints.size();
    const int32_t* metadata = nullptr;
    if (handle.ints.size() >= kC2ExtraInts) {
        const size_t extra = handle.ints.size() - kC2ExtraInts;
        if (handle.ints[extra + 10] == kC2Magic) {
            baseInts = extra;
            metadata = handle.ints.data() + extra;
        }
    }
    if (baseInts < 6) return false;
    int width = metadata ? metadata[0] : handle.ints[0];
    int height = metadata ? metadata[1] : handle.ints[1];
    int format = metadata ? metadata[2] : handle.ints[3];
    int stride = metadata ? metadata[5] : handle.ints[2];
    // gfxstream buffers carry the minigbm layout (graphics/stub/buffer_handle.h).
    TeboxBufferInfo tb;
    if (!metadata && teboxBufferInfoFromInts(handle.ints.data(), baseInts, &tb) && tb.gfxstream) {
        width = tb.width;
        height = tb.height;
        format = tb.format;
        stride = tb.stride;
    }
    if (width <= 0 || height <= 0 || stride < width ||
        format != AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM)
        return false;
    out->fd = handle.fds[0].get();
    out->width = static_cast<uint32_t>(width);
    out->height = static_cast<uint32_t>(height);
    out->stride = static_cast<uint32_t>(stride);
    out->format = static_cast<uint32_t>(format);
    return out->fd >= 0;
}

}  // namespace gki
