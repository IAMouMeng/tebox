#define LOG_TAG "AHAL_Config"
#include <android-base/logging.h>

#include <aidl/android/media/audio/common/AudioHalVolumeCurve.h>
#include <aidl/android/media/audio/common/AudioHalVolumeGroup.h>

#include "core-impl/Config.h"

using aidl::android::media::audio::common::AudioHalEngineConfig;
using aidl::android::media::audio::common::AudioHalVolumeCurve;
using aidl::android::media::audio::common::AudioHalVolumeGroup;

namespace aidl::android::hardware::audio::core {

namespace {
AudioHalVolumeGroup makeVolumeGroup(const char* name) {
    AudioHalVolumeGroup group;
    group.name = name;
    // Let AudioService keep the public stream step counts (MUSIC 0–15, etc.).
    group.minIndex = AudioHalVolumeGroup::INDEX_DEFERRED_TO_AUDIO_SERVICE;
    group.maxIndex = AudioHalVolumeGroup::INDEX_DEFERRED_TO_AUDIO_SERVICE;
    AudioHalVolumeCurve curve;
    curve.deviceCategory = AudioHalVolumeCurve::DeviceCategory::SPEAKER;
    curve.curvePoints = {
            AudioHalVolumeCurve::CurvePoint{.index = 0, .attenuationMb = -9600},
            AudioHalVolumeCurve::CurvePoint{.index = 100, .attenuationMb = 0},
    };
    group.volumeCurves.push_back(std::move(curve));
    return group;
}
}  // namespace

ndk::ScopedAStatus Config::getSurroundSoundConfig(SurroundSoundConfig* _aidl_return) {
    *_aidl_return = {};
    LOG(DEBUG) << __func__ << ": empty surround config (no xsdc)";
    return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus Config::getEngineConfig(AudioHalEngineConfig* _aidl_return) {
    static const AudioHalEngineConfig kConfig = []() {
        AudioHalEngineConfig config;
        config.volumeGroups = {
                makeVolumeGroup("AUDIO_STREAM_VOICE_CALL"),
                makeVolumeGroup("AUDIO_STREAM_SYSTEM"),
                makeVolumeGroup("AUDIO_STREAM_RING"),
                makeVolumeGroup("AUDIO_STREAM_MUSIC"),
                makeVolumeGroup("AUDIO_STREAM_ALARM"),
                makeVolumeGroup("AUDIO_STREAM_NOTIFICATION"),
                makeVolumeGroup("AUDIO_STREAM_DTMF"),
                makeVolumeGroup("AUDIO_STREAM_ACCESSIBILITY"),
        };
        return config;
    }();
    *_aidl_return = kConfig;
    LOG(DEBUG) << __func__ << ": volume groups " << kConfig.volumeGroups.size()
               << " (indices deferred to AudioService)";
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::audio::core
