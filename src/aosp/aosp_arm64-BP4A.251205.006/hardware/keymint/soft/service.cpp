// Use the software KeyMint implementation shipped with this GSI. The default
// instance emulates the TEE slot required by keystore2; no hardware security.
#include <AndroidKeyMintDevice.h>
#include <AndroidSecureClock.h>
#include <AndroidSharedSecret.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>
#include <cstdlib>

using namespace aidl::android::hardware::security;

template<class Interface>
void publish(const std::shared_ptr<Interface>& service) {
    const std::string name = std::string(Interface::descriptor) + "/default";
    auto status = AServiceManager_addService(service->asBinder().get(), name.c_str());
    __android_log_print(status == STATUS_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR,
                        "qemu-keymint", "register %s: %d (software emulation)",
                        name.c_str(), status);
    if (status != STATUS_OK) std::abort();
}

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(0);
    auto device = std::static_pointer_cast<keymint::AndroidKeyMintDevice>(
            keymint::CreateKeyMintDevice(keymint::SecurityLevel::TRUSTED_ENVIRONMENT));
    publish(device);
    publish(ndk::SharedRefBase::make<secureclock::AndroidSecureClock>(device));
    publish(ndk::SharedRefBase::make<sharedsecret::AndroidSharedSecret>(device));
    ABinderProcess_joinThreadPool();
    return 1;
}
