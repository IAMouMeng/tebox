#pragma once
#include <optional>
#include <system/audio.h>
#include <aidl/android/media/audio/common/AudioFormatDescription.h>

inline std::optional<audio_format_t> aidl2legacy_AudioFormatDescription_audio_format_t(
        const aidl::android::media::audio::common::AudioFormatDescription&) {
    return AUDIO_FORMAT_PCM_16_BIT;
}
