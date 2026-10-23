/*
 * Copyright (C) 2023 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <vector>

#define LOG_TAG "AHAL_ModuleStub"
#include <Utils.h>
#include <android-base/logging.h>

#include "core-impl/ModuleStub.h"
#include "core-impl/StreamStub.h"

using aidl::android::hardware::audio::common::SinkMetadata;
using aidl::android::hardware::audio::common::SourceMetadata;
using aidl::android::media::audio::common::AudioOffloadInfo;
using aidl::android::media::audio::common::AudioPort;
using aidl::android::media::audio::common::AudioPortConfig;
using aidl::android::media::audio::common::AudioPortExt;
using aidl::android::media::audio::common::MicrophoneInfo;

namespace aidl::android::hardware::audio::core {

ndk::ScopedAStatus ModuleStub::createInputStream(StreamContext&& context,
                                                 const SinkMetadata& sinkMetadata,
                                                 const std::vector<MicrophoneInfo>& microphones,
                                                 std::shared_ptr<StreamIn>* result) {
    const bool isSubmix = getType() == Type::R_SUBMIX;
    return createStreamInstance<StreamInStub>(result, std::move(context), sinkMetadata,
                                              microphones, isSubmix);
}

ndk::ScopedAStatus ModuleStub::createOutputStream(
        StreamContext&& context, const SourceMetadata& sourceMetadata,
        const std::optional<AudioOffloadInfo>& offloadInfo, std::shared_ptr<StreamOut>* result) {
    const bool isSubmix = getType() == Type::R_SUBMIX;
    return createStreamInstance<StreamOutStub>(result, std::move(context), sourceMetadata,
                                               offloadInfo, isSubmix);
}

ndk::ScopedAStatus ModuleStub::populateConnectedDevicePort(AudioPort* audioPort, int32_t) {
    if (audioPort->ext.getTag() != AudioPortExt::device) {
        LOG(ERROR) << __func__ << ": not a device port: " << audioPort->toString();
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    auto& config = getConfig();
    if (auto it = config.connectedProfiles.find(audioPort->id);
        it != config.connectedProfiles.end() && !it->second.empty()) {
        audioPort->profiles = it->second;
        LOG(INFO) << __func__ << ": port " << audioPort->id << " from connectedProfiles, "
                  << audioPort->profiles.size() << " profile(s)";
        return ndk::ScopedAStatus::ok();
    }
    std::vector<AudioRoute*> routes = getAudioRoutesForAudioPortImpl(audioPort->id);
    if (routes.empty()) {
        if (!audioPort->profiles.empty()) return ndk::ScopedAStatus::ok();
        LOG(ERROR) << __func__ << ": no routes for port " << audioPort->id;
        return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
    }
    const auto& route = *routes.begin();
    AudioPort mixPort;
    ndk::ScopedAStatus status;
    if (route->sinkPortId == audioPort->id) {
        if (route->sourcePortIds.empty()) {
            return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
        }
        status = getAudioPort(*route->sourcePortIds.begin(), &mixPort);
    } else {
        status = getAudioPort(route->sinkPortId, &mixPort);
    }
    if (!status.isOk()) return status;
    if (!mixPort.profiles.empty()) audioPort->profiles = mixPort.profiles;
    LOG(INFO) << __func__ << ": port " << audioPort->id << " from mix " << mixPort.id << ", "
              << audioPort->profiles.size() << " profile(s)";
    return ndk::ScopedAStatus::ok();
}

}  // namespace aidl::android::hardware::audio::core
