#pragma once

#include <aidl/android/hardware/audio/core/BnConfig.h>

namespace aidl::android::hardware::audio::core {

class Config : public BnConfig {
  public:
    Config() = default;

  private:
    ndk::ScopedAStatus getSurroundSoundConfig(SurroundSoundConfig* _aidl_return) override;
    ndk::ScopedAStatus getEngineConfig(
            aidl::android::media::audio::common::AudioHalEngineConfig* _aidl_return) override;
};

}  // namespace aidl::android::hardware::audio::core
