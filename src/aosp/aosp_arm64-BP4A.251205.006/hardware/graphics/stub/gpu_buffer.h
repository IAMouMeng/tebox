#pragma once

#include <gbm.h>
#include <drm/virtgpu_drm.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <unistd.h>

static bool qemuGpuHas3D(int fd) {
    uint64_t value = 0;
    drm_virtgpu_getparam param = {VIRTGPU_PARAM_3D_FEATURES,
                                reinterpret_cast<uint64_t>(&value)};
    return ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &param) == 0 && value != 0;
}

// Each process owns its GBM device. Callers serialize BO operations; the
// initialization itself must also be safe across Binder/RenderThread callers.
static pthread_once_t qemu_gbm_once = PTHREAD_ONCE_INIT;
static gbm_device* qemu_gbm_device;
static bool qemu_gbm_virgl;

static void qemuInitGbm() {
    int fd = open("/dev/dri/renderD128", O_RDWR | O_CLOEXEC);
    if (fd < 0) return;
    qemu_gbm_virgl = qemuGpuHas3D(fd);
    if (!qemu_gbm_virgl) {
        close(fd);
        return;
    }
    // Mesa's cross-build prefix is a host path, not a guest library path.
    setenv("GBM_BACKENDS_PATH", "/vendor/lib64/gbm", 1);
    qemu_gbm_device = gbm_create_device(fd);
    if (!qemu_gbm_device) close(fd);
}

static gbm_device* qemuGbmDevice() {
    pthread_once(&qemu_gbm_once, qemuInitGbm);
    return qemu_gbm_device;
}

static bool qemuGbmUsesVirgl() {
    pthread_once(&qemu_gbm_once, qemuInitGbm);
    return qemu_gbm_virgl;
}
