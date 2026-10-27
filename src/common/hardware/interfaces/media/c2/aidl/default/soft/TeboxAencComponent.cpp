#include "TeboxAencComponent.h"

#include "C2Blocks.h"
#include "C2Status.h"
#include "ComponentInterface.h"
#include "Configurable.h"
#include "FdIo.h"
#include "VirtioSerial.h"

#include <aidl/android/hardware/media/c2/FrameData.h>
#include <aidl/android/hardware/media/c2/Status.h>
#include <aidl/android/hardware/media/c2/Work.h>
#include <aidl/android/hardware/media/c2/WorkBundle.h>
#include <android/log.h>
#include <sys/system_properties.h>

#include <cstring>
#include <mutex>

#undef LOG_TAG
#define LOG_TAG "tebox-aenc"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using aidl::android::hardware::common::NativeHandle;
using aidl::android::hardware::media::c2::BaseBlock;
using aidl::android::hardware::media::c2::Buffer;
using aidl::android::hardware::media::c2::FrameData;
using aidl::android::hardware::media::c2::IComponent;
using aidl::android::hardware::media::c2::IComponentInterface;
using aidl::android::hardware::media::c2::IComponentListener;
using aidl::android::hardware::media::c2::IInputSink;
using aidl::android::hardware::media::c2::IInputSurface;
using aidl::android::hardware::media::c2::IInputSurfaceConnection;
using aidl::android::hardware::media::c2::Status;
using aidl::android::hardware::media::c2::Work;
using aidl::android::hardware::media::c2::WorkBundle;
using ndk::ScopedAStatus;

namespace gki {
namespace {

constexpr uint32_t kAencMagic = 0x434e4541u;  // 'AENC'
enum : uint32_t {
    kAencConfig = 1,
    kAencPcm = 2,
    kAencClose = 3,
    kAencBitstream = 16,
};

struct __attribute__((packed)) AencHdr {
    uint32_t magic, type, size;
};
struct __attribute__((packed)) AencConfig {
    uint32_t session, sample_rate, channels, bitrate, codec;
};
struct __attribute__((packed)) AencPcm {
    uint32_t session;
    uint64_t index;
    int64_t pts_us;
    uint32_t nbytes;
};
struct __attribute__((packed)) AencOut {
    uint32_t session;
    uint64_t index;
    int64_t pts_us;
    int32_t status;
    uint32_t flags;
};

std::mutex gPortMu;
int gPortFd = -1;
uint32_t gNextSession = 1;

bool sendMsg(int fd, uint32_t type, const void* payload, uint32_t size) {
    AencHdr hdr = {kAencMagic, type, size};
    return writeFull(fd, &hdr, sizeof(hdr)) && (!size || writeFull(fd, payload, size));
}

}  // namespace

bool TeboxAencComponent::available() {
    char value[PROP_VALUE_MAX] = "";
    __system_property_get("ro.boot.tebox.gpu", value);
    if (strcmp(value, "gfxstream") != 0) return false;
    std::lock_guard<std::mutex> lock(gPortMu);
    if (gPortFd >= 0) return true;
    const int fd = openVirtioPort("tebox.aenc");
    if (fd < 0) return false;
    gPortFd = fd;
    return true;
}

TeboxAencComponent::TeboxAencComponent(std::string name, uint32_t aencCodec,
                                     std::shared_ptr<ComponentInterface> intf,
                                     std::shared_ptr<Configurable> cfg,
                                     std::shared_ptr<IComponentListener> listener)
    : mName(std::move(name)),
      mAencCodec(aencCodec),
      mIntf(std::move(intf)),
      mCfg(std::move(cfg)),
      mListener(std::move(listener)) {}

TeboxAencComponent::~TeboxAencComponent() {
    release();
}

std::vector<uint8_t> TeboxAencComponent::extractPcm(
        const Buffer& buf, const std::vector<BaseBlock>& baseBlocks) {
    for (const auto& block : buf.blocks) {
        if (block.index < 0 || static_cast<size_t>(block.index) >= baseBlocks.size()) {
            continue;
        }
        std::vector<uint8_t> pcm = readLinearBlock(baseBlocks[static_cast<size_t>(block.index)]);
        if (!pcm.empty()) return pcm;
    }
    return {};
}

void TeboxAencComponent::closeSession() {
    if (mHostFd >= 0 && mSession) {
        sendMsg(mHostFd, kAencClose, &mSession, sizeof(mSession));
    }
    mHostFd = -1;
    mSession = 0;
}

ScopedAStatus TeboxAencComponent::getInterface(std::shared_ptr<IComponentInterface>* out) {
    *out = mIntf;
    return ScopedAStatus::ok();
}

ScopedAStatus TeboxAencComponent::start() {
    if (mReleased) return c2BadState();
    if (mStarted) return ScopedAStatus::ok();
    if (!available()) {
        return ScopedAStatus::fromServiceSpecificError(Status::CANNOT_DO);
    }
    int fd;
    uint32_t session;
    {
        std::lock_guard<std::mutex> lock(gPortMu);
        fd = gPortFd;
        session = gNextSession++;
    }
    AencConfig cfg = {session, 48000, 2, 128000, mAencCodec};
    if (fd < 0 || !sendMsg(fd, kAencConfig, &cfg, sizeof(cfg))) {
        ALOGE("%s: tebox.aenc unavailable", mName.c_str());
        return ScopedAStatus::fromServiceSpecificError(Status::CANNOT_DO);
    }
    mHostFd = fd;
    mSession = session;
    mIndex = 0;
    mStarted = true;
    ALOGI("%s started session=%u codec=%u", mName.c_str(), session, mAencCodec);
    return ScopedAStatus::ok();
}

ScopedAStatus TeboxAencComponent::stop() {
    closeSession();
    mStarted = false;
    return ScopedAStatus::ok();
}

ScopedAStatus TeboxAencComponent::reset() {
    return stop();
}

ScopedAStatus TeboxAencComponent::release() {
    if (mReleased.exchange(true)) return ScopedAStatus::ok();
    return stop();
}

ScopedAStatus TeboxAencComponent::queue(const WorkBundle& workBundle) {
    if (!mStarted) return c2BadState();
    std::lock_guard<std::mutex> lock(mLock);
    for (const Work& src : workBundle.works) {
        WorkBundle done;
        Work out;
        out.chainInfo = src.chainInfo;
        out.input.flags = src.input.flags;
        out.input.ordinal = src.input.ordinal;
        out.worklets.resize(1);
        out.workletsProcessed = 1;
        out.result.status = Status::OK;
        auto& wl = out.worklets[0];
        wl.output = FrameData{};
        wl.output.ordinal = src.input.ordinal;
        const bool eos = (src.input.flags & FrameData::END_OF_STREAM) != 0;
        std::vector<uint8_t> pcm;
        if (!src.input.buffers.empty()) {
            pcm = extractPcm(src.input.buffers[0], workBundle.baseBlocks);
        }
        if (pcm.empty() && !eos) {
            ALOGW("%s: empty PCM (flags=%u buffers=%zu base=%zu)", mName.c_str(),
                  src.input.flags, src.input.buffers.size(), workBundle.baseBlocks.size());
        } else if (!pcm.empty()) {
            AencPcm hdr = {mSession, mIndex++, src.input.ordinal.timestampUs,
                           static_cast<uint32_t>(pcm.size())};
            AencHdr msg = {kAencMagic, kAencPcm,
                           static_cast<uint32_t>(sizeof(hdr) + pcm.size())};
            if (!writeFull(mHostFd, &msg, sizeof(msg)) ||
                !writeFull(mHostFd, &hdr, sizeof(hdr)) ||
                !writeFull(mHostFd, pcm.data(), pcm.size())) {
                out.result.status = Status::CORRUPTED;
            } else {
                AencHdr rh;
                AencOut reply;
                if (!readFull(mHostFd, &rh, sizeof(rh)) || rh.magic != kAencMagic ||
                    rh.type != kAencBitstream || rh.size < sizeof(reply) ||
                    !readFull(mHostFd, &reply, sizeof(reply))) {
                    out.result.status = Status::CORRUPTED;
                } else {
                    std::vector<uint8_t> encoded(rh.size - sizeof(reply));
                    if (!encoded.empty() && !readFull(mHostFd, encoded.data(), encoded.size())) {
                        out.result.status = Status::CORRUPTED;
                    } else if (reply.status < 0) {
                        if (!eos) out.result.status = Status::CORRUPTED;
                    } else if (!encoded.empty()) {
                        if (!makeOutputBlock(encoded, &done.baseBlocks, &wl.output)) {
                            out.result.status = Status::CORRUPTED;
                        } else {
                            wl.output.ordinal.timestampUs = reply.pts_us;
                        }
                    }
                }
            }
        }
        wl.output.flags = eos ? FrameData::END_OF_STREAM : 0;
        done.works.push_back(std::move(out));
        if (mListener) mListener->onWorkDone(done);
    }
    return ScopedAStatus::ok();
}

ScopedAStatus TeboxAencComponent::flush(WorkBundle* out) {
    out->works.clear();
    out->baseBlocks.clear();
    return ScopedAStatus::ok();
}

ScopedAStatus TeboxAencComponent::drain(bool /*withEos*/) {
    return ScopedAStatus::ok();
}

ScopedAStatus TeboxAencComponent::createBlockPool(
        const IComponent::BlockPoolAllocator& /*allocator*/, IComponent::BlockPool* out) {
    out->blockPoolId = mNextBlockPoolId++;
    out->configurable = mCfg;
    return ScopedAStatus::ok();
}

ScopedAStatus TeboxAencComponent::destroyBlockPool(int64_t /*blockPoolId*/) {
    return ScopedAStatus::ok();
}

ScopedAStatus TeboxAencComponent::configureVideoTunnel(int32_t /*avSyncHwId*/,
                                                     NativeHandle* /*out*/) {
    return c2Omitted();
}

ScopedAStatus TeboxAencComponent::connectToInputSurface(
        const std::shared_ptr<IInputSurface>& /*inputSurface*/,
        std::shared_ptr<IInputSurfaceConnection>* /*out*/) {
    return c2Omitted();
}

ScopedAStatus TeboxAencComponent::asInputSink(std::shared_ptr<IInputSink>* out) {
    *out = nullptr;
    return c2Omitted();
}

}  // namespace gki
