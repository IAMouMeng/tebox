#include "HostDecComponent.h"

#include "C2AidlDup.h"
#include "C2Blocks.h"
#include "C2ParamBlob.h"
#include "C2Status.h"
#include "ColorConvert.h"
#include "ComponentInterface.h"
#include "Configurable.h"
#include "FdIo.h"
#include "VdecPort.h"
#include "VdecWire.h"
#include "VirtGpuResource.h"

#include <aidl/android/hardware/media/c2/FrameData.h>
#include <aidl/android/hardware/media/c2/Status.h>
#include <aidl/android/hardware/media/c2/Work.h>
#include <aidl/android/hardware/media/c2/WorkBundle.h>
#include <android/hardware_buffer_aidl.h>
#include <android/log.h>

#include <cstring>
#include <map>
#include <mutex>

#undef LOG_TAG
#define LOG_TAG "tebox-vdec"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)


using aidl::android::hardware::common::NativeHandle;
using aidl::android::hardware::HardwareBuffer;
using aidl::android::hardware::media::c2::BaseBlock;
using aidl::android::hardware::media::c2::Block;
using aidl::android::hardware::media::c2::Buffer;
using aidl::android::hardware::media::c2::FrameData;
using aidl::android::hardware::media::c2::IComponent;
using aidl::android::hardware::media::c2::IComponentInterface;
using aidl::android::hardware::media::c2::IComponentListener;
using aidl::android::hardware::media::c2::IGraphicBufferAllocator;
using aidl::android::hardware::media::c2::IInputSink;
using aidl::android::hardware::media::c2::IInputSurface;
using aidl::android::hardware::media::c2::IInputSurfaceConnection;
using aidl::android::hardware::media::c2::Params;
using aidl::android::hardware::media::c2::Status;
using aidl::android::hardware::media::c2::Work;
using aidl::android::hardware::media::c2::WorkBundle;
using ndk::ScopedAStatus;

namespace gki {

namespace {

constexpr size_t kTargets = 8;
constexpr size_t kMaxAwaiting = 32;
constexpr int kAllocWaitMs = 1000;
constexpr int kFlushWaitMs = 1000;

}  // namespace

bool HostDecComponent::available() { return vdecAvailable(); }

HostDecComponent::HostDecComponent(std::string name, uint32_t codec,
                                   std::shared_ptr<ComponentInterface> intf,
                                   std::shared_ptr<Configurable> cfg,
                                   std::shared_ptr<IComponentListener> listener)
    : mName(std::move(name)),
      mCodec(codec),
      mIntf(std::move(intf)),
      mCfg(std::move(cfg)),
      mListener(std::move(listener)) {}

HostDecComponent::~HostDecComponent() { release(); }

ScopedAStatus HostDecComponent::getInterface(std::shared_ptr<IComponentInterface>* out) {
    *out = mIntf;
    return ScopedAStatus::ok();
}

ScopedAStatus HostDecComponent::start() {
    std::unique_lock<std::mutex> lock(mLock);
    if (mStarted) return ScopedAStatus::ok();
    if (!vdecEnsure()) return ScopedAStatus::fromServiceSpecificError(Status::CANNOT_DO);
    mSession = vdecAllocSession([this](uint32_t type, const uint8_t* payload, size_t size) {
        if (type == kVdecPicture && size >= sizeof(VdecPicture)) {
            onPicture(payload);
        } else if (type == kVdecFlushed) {
            std::lock_guard<std::mutex> lock(mLock);
            mFlushed = true;
            mCv.notify_all();
        }
    });
    const VdecConfig cfg = {mSession, mCodec, mCfg->width(), mCfg->height()};
    if (!vdecSend(kVdecConfig, &cfg, sizeof(cfg))) {
        ALOGE("%s: tebox.vdec unavailable (%s)", mName.c_str(), strerror(errno));
        vdecFreeSession(mSession);
        mSession = 0;
        return ScopedAStatus::fromServiceSpecificError(Status::CANNOT_DO);
    }
    mStarted = mRunning = true;
    mHostFailed = false;
    mWidth = mHeight = 0;
    mWorker = std::thread([this] { workerLoop(); });
    ALOGI("%s started (%ux%u hint)", mName.c_str(), cfg.width, cfg.height);
    return ScopedAStatus::ok();
}

void HostDecComponent::closeSession() {
    std::vector<Target> targets;
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (!mStarted) return;
        mRunning = false;
        mStarted = false;
        for (auto& [id, target] : mTargets) targets.push_back(target);
        mTargets.clear();
        mQueued.clear();
        mAwaiting.clear();
        mCsd.clear();
        mSavedCsd.clear();
    }
    mCv.notify_all();
    vdecFreeSession(mSession);
    vdecSend(kVdecClose, &mSession, sizeof(mSession));
    mSession = 0;
    if (mWorker.joinable()) mWorker.join();
    for (Target& t : targets) releaseTarget(&t, true);
}

ScopedAStatus HostDecComponent::stop() {
    closeSession();
    return ScopedAStatus::ok();
}

ScopedAStatus HostDecComponent::reset() {
    closeSession();
    return ScopedAStatus::ok();
}

ScopedAStatus HostDecComponent::release() {
    closeSession();
    std::lock_guard<std::mutex> lock(mLock);
    mIgba.reset();
    if (mWaitableFd >= 0) close(mWaitableFd);
    mWaitableFd = -1;
    return ScopedAStatus::ok();
}

ScopedAStatus HostDecComponent::queue(const WorkBundle& bundle) {
    std::lock_guard<std::mutex> lock(mLock);
    if (!mStarted) return c2BadState();
    // AIDL owns the incoming descriptors; duplicate what outlives this call.
    for (const Work& src : bundle.works) {
        PendingWork pending;
        pending.work.chainInfo = src.chainInfo;
        pending.work.input = duplicateFrameData(src.input);
        pending.work.worklets.resize(1);
        if (!src.worklets.empty()) pending.work.worklets[0].componentId = src.worklets[0].componentId;
        pending.work.workletsProcessed = 0;
        pending.work.result.status = Status::OK;
        for (const BaseBlock& block : bundle.baseBlocks)
            pending.baseBlocks.push_back(duplicateBaseBlock(block));
        mQueued.push_back(std::move(pending));
    }
    mCv.notify_all();
    return ScopedAStatus::ok();
}

bool HostDecComponent::allocateTarget(Target* target) {
    const uint32_t w = mCfg->width() ? mCfg->width() : 1920;
    const uint32_t h = mCfg->height() ? mCfg->height() : 1088;
    std::shared_ptr<IGraphicBufferAllocator> igba;
    int waitable;
    {
        std::lock_guard<std::mutex> lock(mLock);
        igba = mIgba;
        waitable = mWaitableFd;
    }
    // The host renders into it on the GPU; the consumer samples or scans it.
    const uint64_t usage = AHARDWAREBUFFER_USAGE_GPU_SAMPLED_IMAGE |
                           AHARDWAREBUFFER_USAGE_GPU_FRAMEBUFFER;
    AHardwareBuffer* ahb = nullptr;
    int fence = -1;
    if (igba) {
        // Surface output: buffers come from the consumer's queue.
        IGraphicBufferAllocator::Description desc;
        desc.width = static_cast<int32_t>(w);
        desc.height = static_cast<int32_t>(h);
        desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage = static_cast<int64_t>(usage);
        for (int attempt = 0; attempt < 2 && !ahb; ++attempt) {
            IGraphicBufferAllocator::Allocation alloc;
            ScopedAStatus st = igba->allocate(desc, &alloc);
            if (st.isOk() && alloc.buffer) {
                ahb = alloc.buffer.release();
                if (alloc.fence.get() >= 0) fence = dup(alloc.fence.get());
            } else if (st.getServiceSpecificError() == static_cast<int>(Status::BLOCKING) ||
                       st.getServiceSpecificError() == static_cast<int>(Status::TIMED_OUT)) {
                if (!waitFd(waitable, kAllocWaitMs)) return false;
            } else {
                return false;
            }
        }
        if (!ahb) return false;
        target->fromPool = true;
    } else {
        // ByteBuffer output: our own buffer, readable by the client.
        AHardwareBuffer_Desc desc = {};
        desc.width = w;
        desc.height = h;
        desc.layers = 1;
        desc.format = AHARDWAREBUFFER_FORMAT_R8G8B8A8_UNORM;
        desc.usage = usage | AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN;
        if (AHardwareBuffer_allocate(&desc, &ahb) != 0) return false;
        target->fromPool = false;
    }
    if (fence >= 0) {
        // The consumer may still read it: the host must not draw before.
        waitFd(fence, kAllocWaitMs);
        close(fence);
    }
    target->buffer = ahb;
    AHardwareBuffer_getId(ahb, &target->bufferId);
    target->width = w;
    target->height = h;
    return true;
}

void HostDecComponent::releaseTarget(Target* target, bool returnToPool) {
    if (!target->buffer) return;
    if (returnToPool && target->fromPool) {
        std::shared_ptr<IGraphicBufferAllocator> igba;
        {
            std::lock_guard<std::mutex> lock(mLock);
            igba = mIgba;
        }
        bool ok = false;
        if (igba) igba->deallocate(static_cast<int64_t>(target->bufferId), &ok);
    }
    AHardwareBuffer_release(target->buffer);
    target->buffer = nullptr;
}

// Keeps kTargets output buffers with the host.
bool HostDecComponent::provideTargets() {
    for (;;) {
        {
            std::lock_guard<std::mutex> lock(mLock);
            if (!mRunning || mTargets.size() >= kTargets) return true;
        }
        Target target;
        if (!allocateTarget(&target)) return false;
        const native_handle_t* nh = ahardwareBufferNativeHandle(target.buffer);
        const uint32_t resource =
                nh && nh->numFds > 0 ? virtGpuResourceIdFromFd(nh->data[0]) : 0;
        if (!resource) {
            ALOGE("%s: output buffer is not a virtio-gpu resource", mName.c_str());
            {
                std::lock_guard<std::mutex> lock(mLock);
                mHostFailed = true;
            }
            releaseTarget(&target, true);
            return false;
        }
        uint64_t id = 0;
        {
            std::lock_guard<std::mutex> lock(mLock);
            if (mRunning) {
                id = mNextTargetId++;
                mTargets[id] = target;
            }
        }
        if (!id) {  // stopped meanwhile (releaseTarget takes mLock itself)
            releaseTarget(&target, true);
            return true;
        }
        const VdecOutput out = {mSession, id, resource, target.width, target.height};
        if (!vdecSend(kVdecOutput, &out, sizeof(out))) return false;
    }
}

void HostDecComponent::workerLoop() {
    // Keeps the host supplied with output buffers even when no input is
    // left (it holds decoded pictures until one arrives, the last ones and
    // the end of stream included). |starved|: the consumer has every buffer
    // right now; retry shortly instead of spinning.
    bool starved = false;
    while (true) {
        PendingWork pending;
        bool haveWork = false;
        {
            std::unique_lock<std::mutex> lock(mLock);
            auto ready = [&] {
                return !mRunning || !mQueued.empty() || (!starved && mTargets.size() < kTargets);
            };
            if (starved) {
                mCv.wait_for(lock, std::chrono::milliseconds(10), ready);
            } else {
                mCv.wait(lock, ready);
            }
            if (!mRunning) return;
            if (!mQueued.empty()) {
                pending = std::move(mQueued.front());
                mQueued.pop_front();
                haveWork = true;
            }
        }
        starved = !provideTargets();
        if (!haveWork) continue;
        std::vector<uint8_t> data;
        const FrameData& in = pending.work.input;
        const bool config = (in.flags & FrameData::CODEC_CONFIG) != 0;
        const bool eos = (in.flags & FrameData::END_OF_STREAM) != 0;
        bool ok = readInputFrame(in, pending.baseBlocks, &data);
        std::vector<PendingWork> done;
        if (ok && config) {
            // Parameter sets travel with the next frame (one access unit per
            // host buffer); the config work itself has no output.
            std::lock_guard<std::mutex> lock(mLock);
            mCsd.insert(mCsd.end(), data.begin(), data.end());
            mSavedCsd = mCsd;
            pending.work.workletsProcessed = 1;
            pending.work.worklets[0].output.ordinal = in.ordinal;
            pending.work.worklets[0].output.flags = 0;
            done.push_back(std::move(pending));
        } else if (ok && (!data.empty() || eos)) {
            const VdecInput hdr = {mSession, static_cast<uint64_t>(in.ordinal.frameIndex),
                                   in.ordinal.timestampUs, eos ? kVdecFlagEos : 0u};
            const int64_t frameIndex = in.ordinal.frameIndex;
            {
                std::lock_guard<std::mutex> lock(mLock);
                if (!mCsd.empty() && !data.empty()) {
                    data.insert(data.begin(), mCsd.begin(), mCsd.end());
                    mCsd.clear();
                }
                mAwaiting[hdr.index] = std::move(pending);
            }
            // Never hold mLock while writing: the reader needs it to drain
            // the host's replies, and the host waits for those.
            ok = vdecSend(kVdecInput, &hdr, sizeof(hdr), data.data(),
                          static_cast<uint32_t>(data.size()));
            if (ok && mListener) {
                // The bitstream is with the host: Codec2 may reuse the buffer.
                IComponentListener::InputBuffer released;
                released.frameIndex = frameIndex;
                released.arrayIndex = 0;
                mListener->onInputBuffersReleased({released});
            }
            if (!ok) {
                std::lock_guard<std::mutex> lock(mLock);
                auto it = mAwaiting.find(hdr.index);
                if (it != mAwaiting.end()) {
                    it->second.work.result.status = Status::CORRUPTED;
                    it->second.work.workletsProcessed = 1;
                    done.push_back(std::move(it->second));
                    mAwaiting.erase(it);
                }
            }
        } else {
            // Empty frame (or unreadable input): nothing to decode.
            pending.work.workletsProcessed = 1;
            pending.work.result.status = ok ? Status::OK : static_cast<int32_t>(Status::CORRUPTED);
            pending.work.worklets[0].output.ordinal = in.ordinal;
            done.push_back(std::move(pending));
        }
        emit(&done);
    }
}

void HostDecComponent::finishWork(PendingWork* pending, Target* target, uint32_t width,
                                  uint32_t height, bool eos) {
    Work& work = pending->work;
    FrameData& out = work.worklets[0].output;
    out.ordinal = work.input.ordinal;
    out.flags = eos ? FrameData::END_OF_STREAM : 0;
    work.workletsProcessed = 1;
    work.result.status = Status::OK;
    if (target && target->buffer) {
        HardwareBuffer hwb;
        hwb.reset(target->buffer);  // our reference goes to the client
        target->buffer = nullptr;
        pending->baseBlocks.clear();
        pending->baseBlocks.push_back(BaseBlock::make<BaseBlock::hwbBlock>(std::move(hwb)));
        Block block;
        block.index = 0;
        const uint32_t rect[4] = {0, 0, width, height};
        appendParam(&block.meta.params, kRectInfoIndex, rect, sizeof(rect));
        Buffer buffer;
        buffer.blocks.push_back(std::move(block));
        out.buffers.clear();
        out.buffers.push_back(std::move(buffer));
        // The visible size reaches MediaFormat (width/height, crop).
        if (width != mWidth || height != mHeight) {
            mWidth = width;
            mHeight = height;
            const uint32_t size[2] = {width, height};
            const int32_t crop[4] = {0, 0, static_cast<int32_t>(width),
                                     static_cast<int32_t>(height)};
            appendParam(&out.configUpdate.params, kPictureSizeOut, size, sizeof(size));
            appendParam(&out.configUpdate.params, kCropRectOut, crop, sizeof(crop));
        }
    } else {
        pending->baseBlocks.clear();
        out.buffers.clear();
    }
    // Input blocks were already released to the client.
    work.input.buffers.clear();
}

// Works older than |upToIndex| without a picture by now never get one.
void HostDecComponent::finishStale(uint64_t upToIndex) {
    std::vector<PendingWork> done;
    {
        std::lock_guard<std::mutex> lock(mLock);
        while (mAwaiting.size() > kMaxAwaiting && mAwaiting.begin()->first < upToIndex) {
            finishWork(&mAwaiting.begin()->second, nullptr, 0, 0, false);
            done.push_back(std::move(mAwaiting.begin()->second));
            mAwaiting.erase(mAwaiting.begin());
        }
    }
    emit(&done);
}

void HostDecComponent::onPicture(const void* payload) {
    VdecPicture pic;
    memcpy(&pic, payload, sizeof(pic));
    std::vector<PendingWork> done;
    Target undelivered;
    {
        std::lock_guard<std::mutex> lock(mLock);
        if (!mStarted) return;  // a late reply after stop()
        Target target;
        auto t = pic.id ? mTargets.find(pic.id) : mTargets.end();
        if (t != mTargets.end()) {
            target = t->second;
            mTargets.erase(t);
        }
        if (pic.flags & kVdecFlagEos) {
            // End of stream: everything still waiting completes, the last
            // work carries the EOS flag.
            while (!mAwaiting.empty()) {
                auto it = mAwaiting.begin();
                const bool last = std::next(it) == mAwaiting.end();
                const bool eosWork = (it->second.work.input.flags & FrameData::END_OF_STREAM) != 0;
                finishWork(&it->second, nullptr, 0, 0, last || eosWork);
                done.push_back(std::move(it->second));
                mAwaiting.erase(it);
            }
        } else if (pic.status < 0) {
            ALOGW("%s: picture %llu failed (%d)", mName.c_str(),
                  static_cast<unsigned long long>(pic.index), pic.status);
        } else {
            auto it = mAwaiting.find(pic.index);
            if (it != mAwaiting.end() && target.buffer && !target.fromPool) {
                // ByteBuffer mode: hand out a YUV copy, lend the RGBA again.
                Target yuv;
                yuv.buffer = rgbaToYv12(target.buffer, target.width, target.height);
                yuv.width = target.width;
                yuv.height = target.height;
                if (yuv.buffer) {
                    finishWork(&it->second, &yuv, yuv.width, yuv.height, false);
                    done.push_back(std::move(it->second));
                    mAwaiting.erase(it);
                }
            } else if (it != mAwaiting.end() && target.buffer) {
                // The host scales the visible picture into the whole buffer.
                finishWork(&it->second, &target, target.width, target.height, false);
                done.push_back(std::move(it->second));
                mAwaiting.erase(it);
            }
        }
        if (target.buffer) undelivered = target;
    }
    // Not delivered (failed, or its work is gone): back to the consumer; the
    // worker lends a fresh one with the next input.
    releaseTarget(&undelivered, true);
    emit(&done);
    if (!(pic.flags & kVdecFlagEos)) finishStale(pic.index);
    mCv.notify_all();
}

void HostDecComponent::emit(std::vector<PendingWork>* works) {
    if (works->empty() || !mListener) return;
    WorkBundle bundle;
    for (PendingWork& p : *works) {
        const int32_t base = static_cast<int32_t>(bundle.baseBlocks.size());
        for (Buffer& b : p.work.worklets[0].output.buffers) {
            for (Block& blk : b.blocks) blk.index += base;
        }
        for (BaseBlock& bb : p.baseBlocks) bundle.baseBlocks.push_back(std::move(bb));
        bundle.works.push_back(std::move(p.work));
    }
    works->clear();
    mListener->onWorkDone(bundle);
}

ScopedAStatus HostDecComponent::flush(WorkBundle* out) {
    std::vector<Target> targets;
    {
        std::unique_lock<std::mutex> lock(mLock);
        if (!mStarted) return c2BadState();
        mFlushed = false;
        // Not yet sent: return as is.
        for (PendingWork& p : mQueued) {
            const int32_t base = static_cast<int32_t>(out->baseBlocks.size());
            for (Buffer& b : p.work.input.buffers) {
                for (Block& blk : b.blocks) {
                    if (blk.index >= 0) blk.index += base;
                }
            }
            for (BaseBlock& bb : p.baseBlocks) out->baseBlocks.push_back(std::move(bb));
            out->works.push_back(std::move(p.work));
        }
        mQueued.clear();
        // Streams from MP4 carry the parameter sets only in the codec
        // config: decode the first frame after a seek with them again.
        mCsd = mSavedCsd;
    }
    vdecSend(kVdecFlush, &mSession, sizeof(mSession));
    std::unique_lock<std::mutex> lock(mLock);
    mCv.wait_for(lock, std::chrono::milliseconds(kFlushWaitMs), [&] { return mFlushed; });
    // Sent but without a picture: flushed with no output.
    for (auto& [index, p] : mAwaiting) {
        finishWork(&p, nullptr, 0, 0, false);
        out->works.push_back(std::move(p.work));
    }
    mAwaiting.clear();
    // The host gave every buffer back; return them to the consumer.
    for (auto& [id, target] : mTargets) targets.push_back(target);
    mTargets.clear();
    lock.unlock();
    for (Target& t : targets) releaseTarget(&t, true);
    return ScopedAStatus::ok();
}

ScopedAStatus HostDecComponent::drain(bool /*withEos*/) {
    // MediaCodec drains through an END_OF_STREAM work, handled in queue().
    return ScopedAStatus::ok();
}

ScopedAStatus HostDecComponent::createBlockPool(const IComponent::BlockPoolAllocator& allocator,
                                                IComponent::BlockPool* out) {
    std::lock_guard<std::mutex> lock(mLock);
    if (allocator.gbAllocator && allocator.gbAllocator->igba) {
        mIgba = allocator.gbAllocator->igba;
        if (mWaitableFd >= 0) close(mWaitableFd);
        mWaitableFd = dup(allocator.gbAllocator->waitableFd.get());
    }
    out->blockPoolId = mNextBlockPoolId++;
    out->configurable = mCfg;
    return ScopedAStatus::ok();
}

ScopedAStatus HostDecComponent::destroyBlockPool(int64_t /*blockPoolId*/) {
    return ScopedAStatus::ok();
}

ScopedAStatus HostDecComponent::configureVideoTunnel(int32_t, NativeHandle*) {
    return ScopedAStatus::fromServiceSpecificError(Status::OMITTED);
}

ScopedAStatus HostDecComponent::connectToInputSurface(const std::shared_ptr<IInputSurface>&,
                                                      std::shared_ptr<IInputSurfaceConnection>*) {
    return ScopedAStatus::fromServiceSpecificError(Status::OMITTED);
}

ScopedAStatus HostDecComponent::asInputSink(std::shared_ptr<IInputSink>*) {
    return ScopedAStatus::fromServiceSpecificError(Status::OMITTED);
}

}  // namespace gki
