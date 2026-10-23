// IFactory/default without xsdc EffectFactory XML.
#include <aidl/android/hardware/audio/effect/BnFactory.h>
#include <aidl/android/media/audio/common/AudioUuid.h>
#include <android-base/logging.h>

using aidl::android::hardware::audio::effect::BnFactory;
using aidl::android::hardware::audio::effect::Descriptor;
using aidl::android::hardware::audio::effect::IEffect;
using aidl::android::hardware::audio::effect::Processing;
using aidl::android::media::audio::common::AudioUuid;

namespace aidl::android::hardware::audio::effect {

class EmptyFactory final : public BnFactory {
  public:
    ndk::ScopedAStatus queryEffects(const std::optional<AudioUuid>&,
                                    const std::optional<AudioUuid>&,
                                    const std::optional<AudioUuid>&,
                                    std::vector<Descriptor>* out) override {
        out->clear();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus queryProcessing(const std::optional<Processing::Type>&,
                                       std::vector<Processing>* out) override {
        out->clear();
        return ndk::ScopedAStatus::ok();
    }
    ndk::ScopedAStatus createEffect(const AudioUuid&, std::shared_ptr<IEffect>* out) override {
        out->reset();
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
    ndk::ScopedAStatus destroyEffect(const std::shared_ptr<IEffect>&) override {
        return ndk::ScopedAStatus::fromExceptionCode(EX_UNSUPPORTED_OPERATION);
    }
};

}  // namespace aidl::android::hardware::audio::effect

std::shared_ptr<aidl::android::hardware::audio::effect::IFactory> createEffectFactory() {
    return ndk::SharedRefBase::make<aidl::android::hardware::audio::effect::EmptyFactory>();
}
