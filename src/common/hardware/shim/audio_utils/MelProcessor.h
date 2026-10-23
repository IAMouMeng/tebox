#pragma once
#include <cstdint>
#include <vector>
#include <system/audio.h>
#include <utils/RefBase.h>

namespace android::audio_utils {

class MelProcessor {
  public:
    class MelCallback {
      public:
        virtual ~MelCallback() = default;
        virtual void onNewMelValues(const std::vector<float>&, size_t, size_t, audio_port_handle_t,
                                    bool) const {}
        virtual void onMomentaryExposure(float, audio_port_handle_t) const {}
    };
    MelProcessor(uint32_t, uint32_t, audio_format_t, const android::sp<MelCallback>&,
                 audio_port_handle_t, float) {}
    void setOutputRs2UpperBound(float) {}
    void updateAudioFormat(uint32_t, uint32_t, audio_format_t) {}
    void process(const void*, size_t) {}
};

}  // namespace android::audio_utils
