#pragma once

#include <android/hardware_buffer.h>
#include <cutils/native_handle.h>

#include <cstdint>

namespace gki {

/* virtio-gpu resource id for a dma-buf (PRIME → RESOURCE_INFO). */
uint32_t virtGpuResourceIdFromFd(int dmabuf);

/* LLNDK AHardwareBuffer_getNativeHandle via dlsym (not in NDK stubs). */
const native_handle_t* ahardwareBufferNativeHandle(const AHardwareBuffer* buffer);

}  // namespace gki
