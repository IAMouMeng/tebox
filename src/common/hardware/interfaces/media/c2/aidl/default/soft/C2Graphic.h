#pragma once

#include <aidl/android/hardware/common/NativeHandle.h>
#include <android/hardware_buffer_aidl.h>

#include <cstdint>
#include <vector>

namespace gki {

struct NativeGraphicInfo {
    int fd = -1;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t stride = 0;
    uint32_t format = 0;
};

bool getNativeGraphicInfo(const aidl::android::hardware::common::NativeHandle& handle,
                          NativeGraphicInfo* out);

std::vector<uint8_t> readHardwareBuffer(const aidl::android::hardware::HardwareBuffer& hw);
std::vector<uint8_t> readNativeGraphic(const aidl::android::hardware::common::NativeHandle& handle);

}  // namespace gki
