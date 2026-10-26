#pragma once

#include <aidl/android/hardware/media/c2/BnComponent.h>
#include <aidl/android/hardware/media/c2/IComponentListener.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace gki {

class ComponentInterface;
class Configurable;

/* Guest Codec2 audio encoder: PCM in over tebox.aenc; AAC/Opus/FLAC from host. */
class TeboxAencComponent : public aidl::android::hardware::media::c2::BnComponent {
public:
    TeboxAencComponent(std::string name, uint32_t aencCodec,
                      std::shared_ptr<ComponentInterface> intf,
                      std::shared_ptr<Configurable> cfg,
                      std::shared_ptr<aidl::android::hardware::media::c2::IComponentListener>
                              listener);
    ~TeboxAencComponent() override;

    static bool available();

    ndk::ScopedAStatus configureVideoTunnel(
            int32_t avSyncHwId,
            aidl::android::hardware::common::NativeHandle* _aidl_return) override;
    ndk::ScopedAStatus createBlockPool(
            const aidl::android::hardware::media::c2::IComponent::BlockPoolAllocator& allocator,
            aidl::android::hardware::media::c2::IComponent::BlockPool* _aidl_return) override;
    ndk::ScopedAStatus destroyBlockPool(int64_t blockPoolId) override;
    ndk::ScopedAStatus drain(bool withEos) override;
    ndk::ScopedAStatus flush(
            aidl::android::hardware::media::c2::WorkBundle* _aidl_return) override;
    ndk::ScopedAStatus getInterface(
            std::shared_ptr<aidl::android::hardware::media::c2::IComponentInterface>*
                    _aidl_return) override;
    ndk::ScopedAStatus queue(const aidl::android::hardware::media::c2::WorkBundle& workBundle)
            override;
    ndk::ScopedAStatus release() override;
    ndk::ScopedAStatus reset() override;
    ndk::ScopedAStatus start() override;
    ndk::ScopedAStatus stop() override;
    ndk::ScopedAStatus connectToInputSurface(
            const std::shared_ptr<aidl::android::hardware::media::c2::IInputSurface>& inputSurface,
            std::shared_ptr<aidl::android::hardware::media::c2::IInputSurfaceConnection>*
                    _aidl_return) override;
    ndk::ScopedAStatus asInputSink(
            std::shared_ptr<aidl::android::hardware::media::c2::IInputSink>* _aidl_return)
            override;

private:
    void closeSession();
    static std::vector<uint8_t> extractPcm(
            const aidl::android::hardware::media::c2::Buffer& buf,
            const std::vector<aidl::android::hardware::media::c2::BaseBlock>& baseBlocks);

    const std::string mName;
    const uint32_t mAencCodec; /* CAST_AENC_*: 0=aac 1=opus 2=flac */
    std::shared_ptr<ComponentInterface> mIntf;
    std::shared_ptr<Configurable> mCfg;
    std::shared_ptr<aidl::android::hardware::media::c2::IComponentListener> mListener;

    std::mutex mLock;
    std::atomic<bool> mStarted{false};
    std::atomic<bool> mReleased{false};

    int mHostFd = -1;
    uint32_t mSession = 0;
    uint64_t mIndex = 0;
    int64_t mNextBlockPoolId = 1;
};

}  // namespace gki
