#include "mesa_pipe_video.h"

#include "VencPort.h"

#include <android/log.h>

#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include <cstdio>
#include <cstdlib>
#include <mutex>

#include <drm/virtgpu_drm.h>

#include "pipe/p_video_enums.h"
#include "virtio-gpu/virgl_hw.h"
#include "virtio-gpu/virglrenderer_hw.h"

#define LOG_TAG "mesa-pipe-video"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)

namespace gki {
namespace mesa_video {
namespace {

constexpr const char* kRenderNode = "/dev/dri/renderD128";

// Keep the wire values tied to the same Mesa headers used by the DRI build.
// Entry-point values are UNKNOWN=0, BITSTREAM=1, ... ENCODE=4; the old
// hand-written 0/1 mapping silently classified the host encoder as decode.
constexpr uint32_t kPipeAvcBaseline = PIPE_VIDEO_PROFILE_MPEG4_AVC_BASELINE;
constexpr uint32_t kPipeAvcMain = PIPE_VIDEO_PROFILE_MPEG4_AVC_MAIN;
constexpr uint32_t kPipeAvcHigh = PIPE_VIDEO_PROFILE_MPEG4_AVC_HIGH;
constexpr uint32_t kPipeEntrypointBitstream = PIPE_VIDEO_ENTRYPOINT_BITSTREAM;
constexpr uint32_t kPipeEntrypointEncode = PIPE_VIDEO_ENTRYPOINT_ENCODE;

bool envForce() {
    const char* v = getenv("GKI_MESA_VIDEO_FORCE");
    return v && v[0] == '1' && v[1] == '\0';
}

int openRenderFd() {
    return open(kRenderNode, O_RDWR | O_CLOEXEC);
}

bool readVirglCaps(int fd, union virgl_caps* caps) {
    memset(caps, 0, sizeof(*caps));
    drm_virtgpu_get_caps args = {};
    args.cap_set_id = VIRGL_RENDERER_CAPSET_VIRGL2;
    args.cap_set_ver = 2;
    args.addr = reinterpret_cast<uint64_t>(caps);
    args.size = sizeof(*caps);
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &args) == 0) {
        return true;
    }
    // Fall back to capset 1 size if host is older.
    memset(caps, 0, sizeof(*caps));
    args.cap_set_id = VIRGL_RENDERER_CAPSET_VIRGL;
    args.cap_set_ver = 1;
    args.size = sizeof(struct virgl_caps_v1);
    return ioctl(fd, DRM_IOCTL_VIRTGPU_GET_CAPS, &args) == 0;
}

bool profileIsAvc(uint32_t profile) {
    return profile >= kPipeAvcBaseline && profile <= 16 /* HIGH444 */;
}

Caps probeCapsLocked() {
    Caps out;
    if (envForce()) ALOGI("GKI_MESA_VIDEO_FORCE is ignored unless the backend probes successfully");

    int fd = openRenderFd();
    if (fd < 0) {
        ALOGW("open %s failed: %s", kRenderNode, strerror(errno));
        return out;
    }

    uint64_t has3d = 0;
    drm_virtgpu_getparam gp = {VIRTGPU_PARAM_3D_FEATURES,
                               reinterpret_cast<uint64_t>(&has3d)};
    if (ioctl(fd, DRM_IOCTL_VIRTGPU_GETPARAM, &gp) != 0 || !has3d) {
        ALOGW("virtio-gpu 3D features unavailable");
        ::close(fd);
        return out;
    }

    union virgl_caps caps;
    if (!readVirglCaps(fd, &caps)) {
        ALOGW("VIRTGPU_GET_CAPS failed: %s", strerror(errno));
        ::close(fd);
        return out;
    }
    ::close(fd);

    const uint32_t n = caps.v2.num_video_caps;
    if (n == 0 || n > 32) {
        ALOGI("VirGL video caps unavailable (num_video_caps=%u)", n);
        return out;
    }

    for (uint32_t i = 0; i < n; ++i) {
        const virgl_video_caps& vc = caps.v2.video_caps[i];
        if (!profileIsAvc(vc.profile)) continue;
        if (vc.entrypoint == kPipeEntrypointBitstream) {
            out.avc_decode = true;
        } else if (vc.entrypoint == kPipeEntrypointEncode) {
            out.avc_encode = true;
        }
        if (vc.max_width > out.max_width) out.max_width = vc.max_width;
        if (vc.max_height > out.max_height) out.max_height = vc.max_height;
    }
    ALOGI("VirGL video: avc_dec=%d avc_enc=%d max=%ux%u (n=%u)",
          out.avc_decode ? 1 : 0, out.avc_encode ? 1 : 0, out.max_width, out.max_height, n);
    return out;
}

std::mutex gCapsMu;
bool gCapsInit = false;
Caps gCaps;

}  // namespace

const Caps& queryCaps() {
    if (isGfxstreamGpu()) {
        static const Caps kHostCaps = {false, true, 4096, 4096};
        return kHostCaps;
    }
    std::lock_guard<std::mutex> lock(gCapsMu);
    // The HAL is started in parallel with virtio-gpu.  Do not permanently
    // cache an empty capset observed before renderD128 is ready.
    if (!gCapsInit || (!gCaps.avc_decode && !gCaps.avc_encode)) {
        Caps fresh = probeCapsLocked();
        if (fresh.avc_decode || fresh.avc_encode || !gCapsInit) gCaps = fresh;
        gCapsInit = true;
    }
    return gCaps;
}

bool hasAvcDecode() { return queryCaps().avc_decode; }
bool hasAvcEncode() { return queryCaps().avc_encode; }
bool forceVideoEnabled() { return envForce(); }

bool backendAvailable(Entrypoint ep) {
    // The encoder path below has a complete NV12 -> pipe_video_codec bridge.
    // Decoder submission still needs an AVC parameter-set/slice parser, so do
    // not publish a decoder that would accept work and then return ENOSYS.
    if (ep == Entrypoint::kDecode) return false;
    if (isGfxstreamGpu()) {
        // tebox-cast connects to the port at any time; the port existing is
        // what makes this guest offer the encoder.
        const bool ok = vencPortPresent();
        ALOGI("AVC encode via host (tebox.venc): %s", ok ? "ok" : "no port");
        return ok;
    }
    Session probe;
    const bool ok = probe.open(ep, Profile::kAvcMain, 640, 360);
    FILE* trace = fopen("/data/local/tmp/mesa-c2-probe.log", "a");
    if (trace) {
        fprintf(trace, "backend ep=%d result=%d force=%d\\n", static_cast<int>(ep), ok ? 1 : 0,
                forceVideoEnabled() ? 1 : 0);
        fclose(trace);
    }
    ALOGI("AVC encode backend probe: %s", ok ? "ok" : "failed");
    return ok;
}

}  // namespace mesa_video
}  // namespace gki
