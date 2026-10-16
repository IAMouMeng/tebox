// MagicOS: placeholder for Honor's factory HAL
// (vendor.honor.hardware.hwfactoryinterface.IHwFactoryInterface V1, instance
// hwfactoryinterface_hal). system_server reads the preloaded-app lists from
// its OEM info at boot; without the service the HIDL fallback throws
// NoSuchElementException and system_server crash-loops. oeminfo_Read answers
// "no data" (result 0, empty buffer); everything else is "not implemented".
// Codes and layout from vendor.honor.hardware.hwfactoryinterface-V1-java.jar.
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/binder_stability.h>
#include <android/log.h>

#include <cstring>

namespace {

constexpr char kDescriptor[] = "vendor.honor.hardware.hwfactoryinterface.IHwFactoryInterface";
constexpr char kInstance[] =
        "vendor.honor.hardware.hwfactoryinterface.IHwFactoryInterface/hwfactoryinterface_hal";
constexpr char kHash[] = "56cbff01ec196d796b2d7c319d1427e9d9dad883";
constexpr transaction_code_t kOeminfoRead = FIRST_CALL_TRANSACTION + 34;
constexpr transaction_code_t kGetInterfaceHash = FIRST_CALL_TRANSACTION + 16777213;
constexpr transaction_code_t kGetInterfaceVersion = FIRST_CALL_TRANSACTION + 16777214;

void* onCreate(void* args) { return args; }
void onDestroy(void*) {}

binder_status_t writeOk(AParcel* out) {
    AStatus* ok = AStatus_newOk();
    const binder_status_t status = AParcel_writeStatusHeader(out, ok);
    AStatus_delete(ok);
    return status;
}

binder_status_t onTransact(AIBinder*, transaction_code_t code, const AParcel*, AParcel* out) {
    switch (code) {
        case kOeminfoRead: {
            writeOk(out);
            AParcel_writeInt32(out, 1);   // IFacIntByte: non-null
            AParcel_writeInt32(out, 12);  // parcelable size: itself, result, outBuf length
            AParcel_writeInt32(out, 0);   // result
            return AParcel_writeInt32(out, 0);  // outBuf: empty
        }
        case kGetInterfaceVersion:
            writeOk(out);
            return AParcel_writeInt32(out, 1);
        case kGetInterfaceHash:
            writeOk(out);
            return AParcel_writeString(out, kHash, std::strlen(kHash));
        default:
            return STATUS_UNKNOWN_TRANSACTION;
    }
}

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(2);
    AIBinder_Class* cls = AIBinder_Class_define(kDescriptor, onCreate, onDestroy, onTransact);
    AIBinder* binder = AIBinder_new(cls, nullptr);
    AIBinder_markVintfStability(binder);  // declared in the vendor VINTF manifest
    const binder_status_t status = AServiceManager_addService(binder, kInstance);
    __android_log_print(status == STATUS_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "honor-factory",
                        "register %s: %d", kInstance, status);
    if (status != STATUS_OK) return 1;
    ABinderProcess_joinThreadPool();
    return 0;
}
