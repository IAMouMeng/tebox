#include "VirtGpuResource.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <drm/drm.h>
#include <drm/virtgpu_drm.h>

namespace gki {

uint32_t virtGpuResourceIdFromFd(int dmabuf) {
    static int render = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (render < 0 || dmabuf < 0) return 0;
    drm_prime_handle prime = {};
    prime.fd = dmabuf;
    if (ioctl(render, DRM_IOCTL_PRIME_FD_TO_HANDLE, &prime)) return 0;
    drm_virtgpu_resource_info info = {};
    info.bo_handle = prime.handle;
    const int ret = ioctl(render, DRM_IOCTL_VIRTGPU_RESOURCE_INFO, &info);
    drm_gem_close close_handle = {};
    close_handle.handle = prime.handle;
    ioctl(render, DRM_IOCTL_GEM_CLOSE, &close_handle);
    return ret ? 0 : info.res_handle;
}

const native_handle_t* ahardwareBufferNativeHandle(const AHardwareBuffer* buffer) {
    using Fn = const native_handle_t* (*)(const AHardwareBuffer*);
    static Fn fn = reinterpret_cast<Fn>(dlsym(RTLD_DEFAULT, "AHardwareBuffer_getNativeHandle"));
    return fn ? fn(buffer) : nullptr;
}

}  // namespace gki
