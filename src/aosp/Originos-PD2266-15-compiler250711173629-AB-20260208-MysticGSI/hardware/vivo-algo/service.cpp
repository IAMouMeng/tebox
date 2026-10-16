// OriginOS: placeholder for vivo's vendor algorithm HAL
// (vendor.vivo.hardware.vivoalgoservice.IVASAlgoProvider/default, V2).
// com.vivo.vivo3rdalgoservice waits for it without a timeout, and the vivo
// camera and gallery block on that app's content provider meanwhile. There
// are no vivo camera algorithms here: connectService succeeds with a default
// result so the app stops retrying, getVersion answers, and everything else
// is "not implemented", like the AIDL default implementation.
// Transaction codes from vendor.vivo.hardware.vivoalgoservice-V2-ndk.so.
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>

namespace {

constexpr char kDescriptor[] = "vendor.vivo.hardware.vivoalgoservice.IVASAlgoProvider";
constexpr char kInstance[] = "vendor.vivo.hardware.vivoalgoservice.IVASAlgoProvider/default";
constexpr transaction_code_t kGetVersion = FIRST_CALL_TRANSACTION + 0;
constexpr transaction_code_t kConnectService = FIRST_CALL_TRANSACTION + 1;

void* onCreate(void* args) { return args; }
void onDestroy(void*) {}

binder_status_t onTransact(AIBinder*, transaction_code_t code, const AParcel*, AParcel* out) {
    if (code != kGetVersion && code != kConnectService) return STATUS_UNKNOWN_TRANSACTION;
    AStatus* ok = AStatus_newOk();
    AParcel_writeStatusHeader(out, ok);
    AStatus_delete(ok);
    if (code == kGetVersion) return AParcel_writeInt32(out, 2);
    // out VASResultIPC: non-null, then a stable parcelable holding only its
    // size field, so every field keeps its default.
    AParcel_writeInt32(out, 1);
    return AParcel_writeInt32(out, 4);
}

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(2);
    AIBinder_Class* cls = AIBinder_Class_define(kDescriptor, onCreate, onDestroy, onTransact);
    AIBinder* binder = AIBinder_new(cls, nullptr);
    const binder_status_t status = AServiceManager_addService(binder, kInstance);
    __android_log_print(status == STATUS_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "vivo-algo",
                        "register %s: %d", kInstance, status);
    if (status != STATUS_OK) return 1;
    ABinderProcess_joinThreadPool();
    return 0;
}
