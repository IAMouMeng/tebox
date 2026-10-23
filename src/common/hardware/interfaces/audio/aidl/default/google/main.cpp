/*
 * Copyright (C) 2022 The Android Open Source Project
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

#include <cstdlib>
#include <ctime>
#include <utility>
#include <vector>

#define LOG_TAG "AHAL_Main"
#include <android-base/logging.h>
#include <android/binder_ibinder_platform.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>

#include <aidl/android/hardware/audio/effect/IFactory.h>

#include "core-impl/ChildInterface.h"
#include "core-impl/Config.h"
#include "core-impl/Configuration.h"
#include "core-impl/Module.h"

using aidl::android::hardware::audio::core::ChildInterface;
using aidl::android::hardware::audio::core::Config;
using aidl::android::hardware::audio::core::Module;
using aidl::android::hardware::audio::core::internal::getConfiguration;

std::shared_ptr<aidl::android::hardware::audio::effect::IFactory> createEffectFactory();

namespace {

ChildInterface<Module> createModule(const std::string& name,
                                    std::unique_ptr<Module::Configuration>&& config) {
    ChildInterface<Module> result;
    {
        auto moduleType = Module::typeFromString(name);
        if (!moduleType.has_value()) {
            LOG(ERROR) << __func__ << ": module type \"" << name << "\" is not supported";
            return result;
        }
        auto module = Module::createInstance(*moduleType, std::move(config));
        if (module == nullptr) return result;
        result = std::move(module);
    }
    const std::string moduleFqn = std::string().append(Module::descriptor).append("/").append(name);
    binder_status_t status = AServiceManager_addService(result.getBinder(), moduleFqn.c_str());
    if (status != STATUS_OK) {
        LOG(ERROR) << __func__ << ": failed to register service for \"" << moduleFqn << "\"";
        return ChildInterface<Module>();
    }
    LOG(INFO) << "registered " << moduleFqn;
    return result;
}

}  // namespace

int main() {
    std::srand(std::time(nullptr));
    android::base::SetMinimumLogSeverity(::android::base::DEBUG);
    ABinderProcess_setThreadPoolMaxThreadCount(16);
    ABinderProcess_startThreadPool();

    LOG(INFO) << "Init for Audio AIDL HAL (AOSP ModuleStub + virtio PCM)";

    auto config = ndk::SharedRefBase::make<Config>();
    const std::string configFqn = std::string().append(Config::descriptor).append("/default");
    binder_status_t status =
            AServiceManager_addService(config->asBinder().get(), configFqn.c_str());
    if (status != STATUS_OK) {
        LOG(ERROR) << "failed to register service for \"" << configFqn << "\"";
    }

    auto factory = createEffectFactory();
    const std::string factoryFqn =
            std::string().append(aidl::android::hardware::audio::effect::IFactory::descriptor)
                    .append("/default");
    status = AServiceManager_addService(factory->asBinder().get(), factoryFqn.c_str());
    if (status != STATUS_OK) {
        LOG(ERROR) << "failed to register service for \"" << factoryFqn << "\"";
    }

    std::vector<ChildInterface<Module>> moduleInstances;
    // PRIMARY speaker with PCM16 profiles so AudioPolicy opens a MUSIC mix.
    // STUB AFE_PROXY is not a default MUSIC device; speaker-without-profiles
    // leaves APM with policy info size 0. Bluetooth cannot steal the route:
    // IModule getBluetooth* returns null, and software HCI rejects Create
    // Connection / SCO.
    if (auto instance = createModule("default", getConfiguration(Module::Type::DEFAULT));
        instance) {
        moduleInstances.push_back(std::move(instance));
    }
    if (auto instance = createModule("r_submix", getConfiguration(Module::Type::R_SUBMIX));
        instance) {
        moduleInstances.push_back(std::move(instance));
    }

    ABinderProcess_joinThreadPool();
    return EXIT_FAILURE;
}
