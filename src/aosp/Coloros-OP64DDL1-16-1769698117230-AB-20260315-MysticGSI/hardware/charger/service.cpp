/*
 * ColorOS charger HAL for QEMU: vendor.oplus.hardware.charger.ICharger/default
 * (AIDL V10, VINTF stability).
 *
 * oplus-services' OplusBatteryServiceFeature asks this HAL for charger and
 * battery state. Without it every getter falls back to -1, so the battery
 * notify code reads as "all faults" and SystemUI posts "Voltage error: the
 * charging voltage is too high". The answers here match the health HAL
 * stub: AC power, 100 %, 4.2 V, 25 C, 4000 mAh, a genuine battery and no
 * fault. Every other getter returns 0 or "", every setter succeeds (0).
 *
 * The interface is proprietary, so there is no .aidl to generate from: the
 * transaction table (methods.inc) comes from ColorOS's own Java proxy and
 * replies are written by hand in the AIDL wire format (status header, then
 * the return value).
 */

#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_parcel.h>
#include <android/binder_process.h>
#include <android/binder_stability.h>
#include <android/log.h>

#include <cstdint>
#include <cstdio>
#include <cstring>

#define LOG_TAG "oplus-charger-stub"
#define ALOGI(...) __android_log_print(ANDROID_LOG_INFO, LOG_TAG, __VA_ARGS__)
#define ALOGE(...)                                                     \
    do {                                                               \
        __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__);  \
        fprintf(stderr, LOG_TAG " E: ");                               \
        fprintf(stderr, __VA_ARGS__);                                  \
        fprintf(stderr, "\n");                                         \
    } while (0)

namespace {

constexpr char kDescriptor[] = "vendor.oplus.hardware.charger.ICharger";
constexpr char kInstance[] = "vendor.oplus.hardware.charger.ICharger/default";
constexpr int32_t kVersion = 10;
constexpr char kHash[] = "e287420405426b029c9ef0d09dcdb354db9d456f";

// AIDL meta transactions (TRANSACTION_getInterfaceVersion / ...Hash).
constexpr transaction_code_t kTxGetInterfaceVersion = 16777214;
constexpr transaction_code_t kTxGetInterfaceHash = 16777213;

enum Reply { kInt, kString, kParcelable };

struct Method {
    transaction_code_t code;
    const char* name;
    Reply reply;
};

const Method kMethods[] = {
#include "methods.inc"
};

// Values other than 0 / "" (see the header comment).
int32_t intValue(const char* name) {
    struct { const char* name; int32_t value; } kInts[] = {
        {"getBattAuthenticate", 1},      // genuine battery
        {"getPsyBatteryHmac", 1},        // battery authentication passed
        {"getBatteryVoltageNow", 4200},  // mV
        {"getPsyAcOnline", 1},
        {"getPsyBatteryLevel", 100},
        {"getPsyBatteryFcc", 4000},      // mAh
        {"getPsyBatteryRm", 4000},       // mAh remaining
        {"getPsyBatteryTemp", 250},      // 0.1 C
        {"getUIsohValue", 100},          // battery health %
        {"getPsyBatteryNotify", 0},      // no charger / battery fault
    };
    for (const auto& v : kInts) {
        if (!strcmp(v.name, name)) return v.value;
    }
    return 0;
}

const char* stringValue(const char* name) {
    if (!strcmp(name, "getPsyBatteryStatus")) return "Full";
    return "";
}

const Method* find(transaction_code_t code) {
    for (const auto& m : kMethods) {
        if (m.code == code) return &m;
    }
    return nullptr;
}

void* onCreate(void* args) { return args; }
void onDestroy(void*) {}

binder_status_t onTransact(AIBinder*, transaction_code_t code, const AParcel*, AParcel* out) {
    // Status header: no exception.
    if (code == kTxGetInterfaceVersion) {
        AParcel_writeInt32(out, 0);
        return AParcel_writeInt32(out, kVersion);
    }
    if (code == kTxGetInterfaceHash) {
        AParcel_writeInt32(out, 0);
        return AParcel_writeString(out, kHash, sizeof(kHash) - 1);
    }
    const Method* m = find(code);
    if (!m) return STATUS_UNKNOWN_TRANSACTION;
    AParcel_writeInt32(out, 0);
    switch (m->reply) {
        case kInt:
            return AParcel_writeInt32(out, intValue(m->name));
        case kString: {
            const char* s = stringValue(m->name);
            return AParcel_writeString(out, s, static_cast<int32_t>(strlen(s)));
        }
        case kParcelable:
            return AParcel_writeInt32(out, 0);  // null testKitFeatureTestResult
    }
    return STATUS_OK;
}

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(2);
    AIBinder_Class* cls = AIBinder_Class_define(kDescriptor, onCreate, onDestroy, onTransact);
    AIBinder* binder = AIBinder_new(cls, nullptr);
    AIBinder_markVintfStability(binder);
    binder_status_t st = AServiceManager_addService(binder, kInstance);
    if (st != STATUS_OK) {
        ALOGE("addService %s failed: %d", kInstance, st);
        return 1;
    }
    ALOGI("%s ready (V%d, %zu methods)", kInstance, kVersion, sizeof(kMethods) / sizeof(kMethods[0]));
    ABinderProcess_joinThreadPool();
    return 0;
}
