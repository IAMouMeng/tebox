#include "MesaAvcComponent.h"

#include "AvcAnnexB.h"
#include "C2AidlDup.h"
#include "C2Blocks.h"
#include "C2Graphic.h"
#include "C2Status.h"
#include "ComponentInterface.h"
#include "Configurable.h"
#include "FdIo.h"

#include <aidl/android/hardware/media/c2/BnInputSink.h>
#include <aidl/android/hardware/media/c2/BnInputSurfaceConnection.h>
#include <aidl/android/hardware/media/c2/FrameData.h>
#include <aidl/android/hardware/media/c2/IComponentListener.h>
#include <aidl/android/hardware/media/c2/Status.h>
#include <aidl/android/hardware/media/c2/Work.h>
#include <aidl/android/hardware/media/c2/WorkBundle.h>
#include <aidl/android/hardware/media/c2/WorkOrdinal.h>

#include <android/log.h>
#include <errno.h>

#include <atomic>
#include <cstring>
#include <utility>

#define LOG_TAG "mesa-c2"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGW(...) __android_log_print(ANDROID_LOG_WARN, LOG_TAG, __VA_ARGS__)
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

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
using aidl::android::hardware::common::NativeHandle;
using ndk::ScopedAStatus;

namespace gki {

namespace {

class MesaInputSurfaceConnection final
        : public aidl::android::hardware::media::c2::BnInputSurfaceConnection {
public:
    ScopedAStatus disconnect() override { return ScopedAStatus::ok(); }
    ScopedAStatus signalEndOfStream() override { return ScopedAStatus::ok(); }
};

int64_t framePtsUs(const FrameData& frame) {
    if (frame.ordinal.timestampUs != 0) return frame.ordinal.timestampUs;
    return static_cast<int64_t>(frame.ordinal.frameIndex) * 33333;
}

}  // namespace

class MesaInputSink final : public aidl::android::hardware::media::c2::BnInputSink {
public:
    explicit MesaInputSink(MesaAvcComponent* component) : mComponent(component) {}
    ScopedAStatus queue(const WorkBundle& bundle) override {
        return mComponent ? mComponent->queue(bundle) : c2BadState();
    }

private:
    MesaAvcComponent* const mComponent;
};

MesaAvcComponent::MesaAvcComponent(
        MesaCodecKind kind, std::string name, std::shared_ptr<ComponentInterface> intf,
        std::shared_ptr<Configurable> cfg, std::shared_ptr<IComponentListener> listener,
        uint32_t castCodec)
    : mKind(kind),
      mName(std::move(name)),
      mCastCodec(castCodec),
      mIntf(std::move(intf)),
      mCfg(std::move(cfg)),
      mListener(std::move(listener)) {}

MesaAvcComponent::~MesaAvcComponent() {
    // A client that dies without stop()/release() leaves both threads
    // running; a joinable std::thread in a destructor terminates the HAL.
    {
        std::lock_guard<std::mutex> lock(mLock);
        mReleased = true;
        mRunning = false;
    }
    mCv.notify_all();
    mInFlightCv.notify_all();
    if (mWorker.joinable()) mWorker.join();
    if (mCollector.joinable()) mCollector.join();
}

std::vector<uint8_t> MesaAvcComponent::extractLinearData(
        const Buffer& buf, const std::vector<BaseBlock>& baseBlocks) {
    static std::atomic<int> extractDiagnostics{0};
    if (extractDiagnostics.fetch_add(1) < 12) {
        ALOGI("extract buffer blocks=%zu base=%zu", buf.blocks.size(), baseBlocks.size());
        for (const auto& block : buf.blocks) {
            ALOGI("extract block index=%d", block.index);
        }
    }
    for (const auto& block : buf.blocks) {
        if (block.index < 0 || static_cast<size_t>(block.index) >= baseBlocks.size()) {
            continue;
        }
        const BaseBlock& bb = baseBlocks[static_cast<size_t>(block.index)];
        if (bb.getTag() == BaseBlock::nativeBlock) {
            const NativeHandle& nh = bb.get<BaseBlock::nativeBlock>();
            static std::atomic<int> nativeDiagnostics{0};
            if (nativeDiagnostics.fetch_add(1) < 8) {
                ALOGI("extract native fds=%zu ints=%zu", nh.fds.size(), nh.ints.size());
            }
            if (!nh.fds.empty()) {
                if (nh.ints.size() >= 6) {
                    std::vector<uint8_t> graphic = readNativeGraphic(nh);
                    if (!graphic.empty()) return graphic;
                }
                return readFd(nh.fds[0].get(), 16 * 1024 * 1024);
            }
        } else if (bb.getTag() == BaseBlock::hwbBlock) {
            return readHardwareBuffer(bb.get<BaseBlock::hwbBlock>());
        }
    }
    return {};
}

void MesaAvcComponent::processWork(PendingWork* pending) {
    Work* work = &pending->work;
    work->workletsProcessed = 0;
    work->result.status = Status::OK;
    if (work->worklets.empty()) {
        work->worklets.emplace_back();
    }
    auto& wl = work->worklets[0];
    wl.output = FrameData{};
    wl.output.ordinal = work->input.ordinal;

    const bool eos = (work->input.flags & FrameData::END_OF_STREAM) != 0;
    (void)((work->input.flags & FrameData::CODEC_CONFIG) != 0);

    if (mKind == MesaCodecKind::kDecoder) {
        std::vector<uint8_t> bs;
        for (const Buffer& b : work->input.buffers) {
            auto chunk = extractLinearData(b, pending->baseBlocks);
            bs.insert(bs.end(), chunk.begin(), chunk.end());
        }
        std::vector<uint8_t> nv12;
        int64_t pts = 0;
        bool outEos = false;
        int rc = mSession.decode(bs.data(), bs.size(), framePtsUs(work->input), &nv12, &pts,
                                 &outEos);
        if (rc == -ENOSYS) {
            work->result.status = Status::CANNOT_DO;
            return;
        }
        if (rc < 0 && !eos) {
            work->result.status = Status::CORRUPTED;
            return;
        }
        if (rc > 0) makeOutputBlock(nv12, &pending->baseBlocks, &wl.output);
        wl.output.flags = eos ? FrameData::END_OF_STREAM : 0;
        wl.output.ordinal.timestampUs = pts;
        work->workletsProcessed = 1;
        return;
    }

    // Encoders are pipelined: see submitEncode()/finishEncode().
    work->result.status = Status::CANNOT_DO;
}

bool MesaAvcComponent::submitEncode(PendingWork* pending, mesa_video::EncodeTicket* ticket) {
    Work* work = &pending->work;
    work->workletsProcessed = 0;
    work->result.status = Status::OK;
    if (work->worklets.empty()) work->worklets.emplace_back();
    auto& wl = work->worklets[0];
    wl.output = FrameData{};
    wl.output.ordinal = work->input.ordinal;
    // Hardware mode is GPU-only: import the surface dmabuf into VirGL video
    // and encode on the host. Never fall back to mapper CPU readback.
    int rc = -ENOSYS;
    bool submitted = false;
    // The producer (SurfaceFlinger composing a virtual display, a camera...)
    // may still be drawing into the buffer: wait for its acquire fence before
    // the host encoder reads the dma-buf, or it encodes stale content.
    for (const auto& buffer : work->input.buffers) {
        for (const auto& block : buffer.blocks) {
            for (const auto& fd : block.fence.fds) {
                if (fd.get() < 0) continue;
                if (!waitFd(fd.get(), 1000)) ALOGW("encode: input acquire fence did not signal");
            }
        }
    }
    for (const BaseBlock& bb : pending->baseBlocks) {
        if (bb.getTag() != BaseBlock::nativeBlock) continue;
        NativeGraphicInfo info;
        if (!getNativeGraphicInfo(bb.get<BaseBlock::nativeBlock>(), &info)) continue;
        const bool keyframe = mSession.submittedCount() == 0;
        rc = mSession.submitGraphic(info.fd, info.width, info.height, info.stride, info.format,
                                    framePtsUs(work->input), keyframe, ticket);
        submitted = true;
        static std::atomic<int> encodeDiagnostics{0};
        if (encodeDiagnostics.fetch_add(1) < 8) {
            ALOGI("gpu encode submit fd=%d %ux%u stride=%u fmt=%u key=%d rc=%d", info.fd,
                  info.width, info.height, info.stride, info.format, keyframe ? 1 : 0, rc);
        }
        break;
    }
    if (!submitted) {
        static std::atomic<int> missingDiagnostics{0};
        if (missingDiagnostics.fetch_add(1) < 4) {
            ALOGW("%s: no graphic dmabuf in work (buffers=%zu base=%zu)", mName.c_str(),
                  work->input.buffers.size(), pending->baseBlocks.size());
        }
        work->result.status = Status::CORRUPTED;
        return false;
    }
    if (rc < 0) {
        finishEncode(pending, rc, {}, 0, 0);
        return false;
    }
    return true;
}

void MesaAvcComponent::finishEncode(PendingWork* pending, int rc,
                                    const std::vector<uint8_t>& bs, int64_t pts,
                                    uint64_t index) {
    Work* work = &pending->work;
    auto& wl = work->worklets[0];
    const bool eos = (work->input.flags & FrameData::END_OF_STREAM) != 0;
    if (rc == -ENOSYS) {
        work->result.status = Status::CANNOT_DO;
        return;
    }
    if (rc < 0 && !eos) {
        work->result.status = Status::CORRUPTED;
        return;
    }
    if (rc > 0 && !makeOutputBlock(bs, &pending->baseBlocks, &wl.output)) {
        ALOGE("%s: makeOutputBlock failed for %zu bytes", mName.c_str(), bs.size());
        work->result.status = Status::CORRUPTED;
    } else if (rc > 0 && !wl.output.buffers.empty()) {
        // C2StreamPictureTypeInfo::output: MediaCodec derives
        // BUFFER_FLAG_SYNCFRAME from it; MPEG4Writer drops samples until one.
        constexpr uint32_t kPictureTypeIndex = 0xD2001810u;
        constexpr uint32_t kSyncFrame = 1u << 0, kIFrame = 1u << 1, kPFrame = 1u << 2;
        const uint32_t type = avcHasIdr(bs) ? (kSyncFrame | kIFrame) : kPFrame;
        const uint32_t size = 12;
        auto& info = wl.output.buffers.back().info.params;
        info.resize(16, 0);
        memcpy(info.data(), &size, sizeof(size));
        memcpy(info.data() + 4, &kPictureTypeIndex, sizeof(kPictureTypeIndex));
        memcpy(info.data() + 8, &type, sizeof(type));
    }
    wl.output.flags = eos ? FrameData::END_OF_STREAM : 0;
    // First IDR carries SPS/PPS. Publish coded.init-data and mark CODEC_CONFIG
    // so scrcpy / MediaCodec treat the first packet as config, not a slice.
    if (rc > 0 && index == 0) {
        std::vector<uint8_t> csd = extractAvcCsd(bs);
        if (!csd.empty()) {
            constexpr uint32_t kInitDataIndex = 0xD2010809u;
            const uint32_t paramSize = static_cast<uint32_t>(8 + csd.size());
            aidl::android::hardware::media::c2::Params init;
            init.params.resize((paramSize + 7u) & ~7u, 0);
            memcpy(init.params.data(), &paramSize, sizeof(paramSize));
            memcpy(init.params.data() + 4, &kInitDataIndex, sizeof(kInitDataIndex));
            memcpy(init.params.data() + 8, csd.data(), csd.size());
            wl.output.configUpdate = std::move(init);
        } else {
            ALOGW("%s: first frame missing SPS/PPS (%zu bytes)", mName.c_str(), bs.size());
        }
    }
    wl.output.ordinal.timestampUs = pts;
    work->workletsProcessed = 1;
}

void MesaAvcComponent::markDropped(PendingWork* stale) {
    if (stale->work.worklets.empty()) stale->work.worklets.emplace_back();
    stale->work.workletsProcessed = 1;
    stale->work.result.status = Status::OK;
    auto& output = stale->work.worklets[0].output;
    output = FrameData{};
    output.ordinal = stale->work.input.ordinal;
    output.flags = FrameData::DROP_FRAME;
}

void MesaAvcComponent::emitWork(PendingWork* pending) {
    if (!mListener) return;
    WorkBundle bundle;
    bundle.works.push_back(std::move(pending->work));
    bundle.baseBlocks = std::move(pending->baseBlocks);
    mListener->onWorkDone(bundle);
}

void MesaAvcComponent::workerLoop() {
    const bool encoder = mKind == MesaCodecKind::kEncoder;
    while (true) {
        PendingWork pending;
        std::vector<PendingWork> dropped;
        bool havePending = false;
        {
            std::unique_lock<std::mutex> lock(mLock);
            mCv.wait(lock, [&] {
                return mReleased.load() || !mPending.empty() || !mDropped.empty() ||
                       (!mRunning.load() && mPending.empty() && mDropped.empty());
            });
            if (mReleased || (!mRunning && mPending.empty() && mDropped.empty())) break;
            dropped.swap(mDropped);
            if (!mPending.empty()) {
                pending = std::move(mPending.front());
                mPending.erase(mPending.begin());
                havePending = true;
            }
            if (encoder) {
                // Superseded frames complete through the collector so every
                // work finishes in input order.
                for (PendingWork& stale : dropped) {
                    InFlight entry;
                    entry.kind = InFlight::Kind::kDropped;
                    entry.pending = std::move(stale);
                    mInFlight.push_back(std::move(entry));
                }
                if (!dropped.empty()) mInFlightCv.notify_all();
                dropped.clear();
            }
        }
        for (PendingWork& stale : dropped) {
            markDropped(&stale);
            emitWork(&stale);
        }
        if (!havePending) continue;
        if (!encoder) {
            processWork(&pending);
            emitWork(&pending);
            continue;
        }
        {
            // Bound the pipeline: at most kMaxInFlight frames on the host.
            std::unique_lock<std::mutex> lock(mLock);
            mInFlightCv.wait(lock, [&] {
                return mReleased.load() || !mRunning.load() ||
                       mEncodedInFlight < mesa_video::Session::kMaxInFlight;
            });
            if (mReleased || !mRunning) break;
        }
        InFlight entry;
        const bool ok = submitEncode(&pending, &entry.ticket);
        entry.kind = ok ? InFlight::Kind::kEncoded : InFlight::Kind::kDone;
        entry.pending = std::move(pending);
        {
            std::lock_guard<std::mutex> lock(mLock);
            if (ok) ++mEncodedInFlight;
            mInFlight.push_back(std::move(entry));
        }
        mInFlightCv.notify_all();
    }
    {
        std::lock_guard<std::mutex> lock(mLock);
        mWorkerDone = true;
    }
    mInFlightCv.notify_all();
}

void MesaAvcComponent::collectorLoop() {
    while (true) {
        InFlight entry;
        {
            std::unique_lock<std::mutex> lock(mLock);
            mInFlightCv.wait(lock, [&] { return !mInFlight.empty() || mWorkerDone; });
            if (mInFlight.empty()) return;  // worker finished and queue drained
            entry = std::move(mInFlight.front());
            mInFlight.pop_front();
        }
        if (entry.kind == InFlight::Kind::kEncoded) {
            std::vector<uint8_t> bs;
            const int rc = mSession.collect(&entry.ticket, &bs);
            finishEncode(&entry.pending, rc, bs, entry.ticket.ptsUs, entry.ticket.index);
            {
                std::lock_guard<std::mutex> lock(mLock);
                --mEncodedInFlight;
            }
            mInFlightCv.notify_all();
        } else if (entry.kind == InFlight::Kind::kDropped) {
            markDropped(&entry.pending);
        }
        // After stop()/release() the framework no longer expects these.
        if (mRunning && !mReleased) emitWork(&entry.pending);
    }
}

ScopedAStatus MesaAvcComponent::getInterface(std::shared_ptr<IComponentInterface>* out) {
    *out = mIntf;
    return ScopedAStatus::ok();
}

ScopedAStatus MesaAvcComponent::start() {
    if (mReleased) return c2BadState();
    if (mStarted) return ScopedAStatus::ok();
    mesa_video::Entrypoint ep = mKind == MesaCodecKind::kDecoder
                                        ? mesa_video::Entrypoint::kDecode
                                        : mesa_video::Entrypoint::kEncode;
    if (!mSession.open(ep, mesa_video::Profile::kAvcMain, mCfg->width(), mCfg->height(),
                       mCastCodec)) {
        return ScopedAStatus::fromServiceSpecificError(Status::CANNOT_DO);
    }
    mRunning = true;
    mStarted = true;
    {
        std::lock_guard<std::mutex> lock(mLock);
        mWorkerDone = false;
    }
    if (!mWorker.joinable()) {
        mWorker = std::thread([this] { workerLoop(); });
    }
    if (mKind == MesaCodecKind::kEncoder && !mCollector.joinable()) {
        mCollector = std::thread([this] { collectorLoop(); });
    }
    ALOGI("%s started %ux%u", mName.c_str(), mCfg->width(), mCfg->height());
    return ScopedAStatus::ok();
}

ScopedAStatus MesaAvcComponent::stop() {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mRunning = false;
        mPending.clear();
        mDropped.clear();
    }
    mCv.notify_all();
    mInFlightCv.notify_all();
    if (mWorker.joinable()) mWorker.join();
    if (mCollector.joinable()) mCollector.join();  // drains in-flight encodes
    mSession.close();
    mStarted = false;
    return ScopedAStatus::ok();
}

ScopedAStatus MesaAvcComponent::reset() {
    stop();
    mSession.flush();
    return ScopedAStatus::ok();
}

ScopedAStatus MesaAvcComponent::release() {
    {
        std::lock_guard<std::mutex> lock(mLock);
        mReleased = true;
        mRunning = false;
        mPending.clear();
        mDropped.clear();
    }
    mCv.notify_all();
    mInFlightCv.notify_all();
    if (mWorker.joinable()) mWorker.join();
    if (mCollector.joinable()) mCollector.join();
    mSession.close();
    return ScopedAStatus::ok();
}

ScopedAStatus MesaAvcComponent::queue(const WorkBundle& workBundle) {
    if (!mStarted) return c2BadState();
    static std::atomic<int> queueDiagnostics{0};
    if (queueDiagnostics.fetch_add(1) < 8) {
        int native = 0, hwb = 0, pooled = 0;
        for (const auto& block : workBundle.baseBlocks) {
            if (block.getTag() == BaseBlock::nativeBlock) ++native;
            else if (block.getTag() == BaseBlock::hwbBlock) ++hwb;
            else if (block.getTag() == BaseBlock::pooledBlock) ++pooled;
        }
        ALOGI("queue works=%zu base=%zu native=%d hwb=%d pooled=%d", workBundle.works.size(),
              workBundle.baseBlocks.size(), native, hwb, pooled);
        for (const auto& block : workBundle.baseBlocks) {
            if (block.getTag() == BaseBlock::nativeBlock) {
                const NativeHandle& nh = block.get<BaseBlock::nativeBlock>();
                ALOGI("native handle fds=%zu ints=%zu [%d,%d,%d,%d,%d,%d]", nh.fds.size(),
                      nh.ints.size(), nh.ints.size() > 0 ? nh.ints[0] : 0,
                      nh.ints.size() > 1 ? nh.ints[1] : 0, nh.ints.size() > 2 ? nh.ints[2] : 0,
                      nh.ints.size() > 3 ? nh.ints[3] : 0, nh.ints.size() > 4 ? nh.ints[4] : 0,
                      nh.ints.size() > 5 ? nh.ints[5] : 0);
                break;
            }
        }
    }
    // Surface input is real-time: once encoding falls behind, processing every
    // queued frame only turns the encoder into a latency buffer. Keep one
    // waiting frame at most and complete superseded frames as DROP_FRAME so
    // Codec2 can release their buffers immediately.
    std::vector<PendingWork> dropped;
    std::unique_lock<std::mutex> lock(mLock);
    // AIDL owns the incoming file descriptors. Duplicate them before the
    // asynchronous worker outlives this binder call.
    for (const Work& src : workBundle.works) {
        const bool control = (src.input.flags &
                              (FrameData::END_OF_STREAM | FrameData::CODEC_CONFIG)) != 0;
        if (mKind == MesaCodecKind::kEncoder && !control) {
            while (!mPending.empty()) {
                dropped.push_back(std::move(mPending.front()));
                mPending.erase(mPending.begin());
            }
        }
        PendingWork pending;
        pending.work = duplicateWork(src);
        if (pending.work.worklets.empty()) pending.work.worklets.resize(1);
        pending.work.workletsProcessed = 0;
        pending.work.result.status = Status::OK;
        pending.baseBlocks.reserve(workBundle.baseBlocks.size());
        for (const auto& block : workBundle.baseBlocks) {
            pending.baseBlocks.push_back(duplicateBaseBlock(block));
        }
        mPending.push_back(std::move(pending));
    }
    for (PendingWork& pending : dropped) mDropped.push_back(std::move(pending));
    lock.unlock();
    mCv.notify_one();
    return ScopedAStatus::ok();
}

ScopedAStatus MesaAvcComponent::flush(WorkBundle* out) {
    std::unique_lock<std::mutex> lock(mLock);
    // Let submitted encodes complete through onWorkDone before returning.
    mInFlightCv.wait(lock, [&] { return mInFlight.empty() || mWorkerDone; });
    out->works.clear();
    out->baseBlocks.clear();
    for (PendingWork& pending : mPending) {
        const int32_t baseOffset = static_cast<int32_t>(out->baseBlocks.size());
        for (auto& block : pending.baseBlocks) out->baseBlocks.push_back(std::move(block));
        for (auto& block : pending.work.input.buffers) {
            for (auto& ref : block.blocks) {
                if (ref.index >= 0) ref.index += baseOffset;
            }
        }
        out->works.push_back(std::move(pending.work));
    }
    mPending.clear();
    mDropped.clear();
    return ScopedAStatus::ok();
}

ScopedAStatus MesaAvcComponent::drain(bool /*withEos*/) {
    // There is no pipe-video drain operation until the backend is available.
    // Returning OK here would make Codec2 wait for output that can never be
    // produced and leaves the component permanently stuck.
    return c2NotSupported();
}

ScopedAStatus MesaAvcComponent::createBlockPool(const IComponent::BlockPoolAllocator& /*allocator*/,
                                                 IComponent::BlockPool* out) {
    out->blockPoolId = mNextBlockPoolId++;
    out->configurable = mCfg;
    return ScopedAStatus::ok();
}

ScopedAStatus MesaAvcComponent::destroyBlockPool(int64_t /*blockPoolId*/) {
    return ScopedAStatus::ok();
}

ScopedAStatus MesaAvcComponent::configureVideoTunnel(int32_t /*avSyncHwId*/, NativeHandle* /*out*/) {
    return c2NotSupported();
}

ScopedAStatus MesaAvcComponent::connectToInputSurface(
        const std::shared_ptr<IInputSurface>& inputSurface,
        std::shared_ptr<IInputSurfaceConnection>* out) {
    if (!inputSurface || !out || !mStarted) return c2BadState();
    // The AIDL input-surface path delivers GraphicBuffer-backed WorkBundles
    // through asInputSink(). Keep the connection alive even though the
    // component has no codec-specific drain operation.
    *out = ndk::SharedRefBase::make<MesaInputSurfaceConnection>();
    return ScopedAStatus::ok();
}

ScopedAStatus MesaAvcComponent::asInputSink(std::shared_ptr<IInputSink>* out) {
    if (!out) return c2BadState();
    ALOGI("asInputSink called");
    *out = ndk::SharedRefBase::make<MesaInputSink>(this);
    return ScopedAStatus::ok();
}

}  // namespace gki
