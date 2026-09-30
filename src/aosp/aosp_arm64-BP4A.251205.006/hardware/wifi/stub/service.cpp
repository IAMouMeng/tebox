// Soft WiFi AIDL v2 with one simulated chip + STA iface (wlan0).
#include <aidl/android/hardware/wifi/BnWifi.h>
#include <aidl/android/hardware/wifi/BnWifiChip.h>
#include <aidl/android/hardware/wifi/BnWifiStaIface.h>
#include <aidl/android/hardware/wifi/IfaceConcurrencyType.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>

#include <array>
#include <mutex>
#include <string>
#include <vector>

using namespace aidl::android::hardware::wifi;
using ndk::ScopedAStatus;

class QemuStaIface : public BnWifiStaIface {
  public:
    explicit QemuStaIface(std::string name) : name_(std::move(name)) {}

    ScopedAStatus getName(std::string* out) override {
        *out = name_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getFeatureSet(int32_t* out) override {
        *out = 0;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getFactoryMacAddress(std::array<uint8_t, 6>* out) override {
        *out = mac_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus setMacAddress(const std::array<uint8_t, 6>& mac) override {
        mac_ = mac;
        return ScopedAStatus::ok();
    }
    ScopedAStatus setScanMode(bool) override { return ScopedAStatus::ok(); }
    ScopedAStatus registerEventCallback(
            const std::shared_ptr<IWifiStaIfaceEventCallback>& cb) override {
        callback_ = cb;
        return ScopedAStatus::ok();
    }

  private:
    std::string name_;
    std::array<uint8_t, 6> mac_{{0x02, 0x00, 0x00, 0x00, 0x00, 0x01}};
    std::shared_ptr<IWifiStaIfaceEventCallback> callback_;
};

class QemuWifiChip : public BnWifiChip {
  public:
    ScopedAStatus configureChip(int32_t modeId) override {
        mode_ = modeId;
        if (callback_) callback_->onChipReconfigured(modeId);
        return ScopedAStatus::ok();
    }
    ScopedAStatus createStaIface(std::shared_ptr<IWifiStaIface>* out) override {
        std::lock_guard lock(mu_);
        if (!sta_) sta_ = ndk::SharedRefBase::make<QemuStaIface>("wlan0");
        *out = sta_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getAvailableModes(std::vector<IWifiChip::ChipMode>* out) override {
        IWifiChip::ChipConcurrencyCombinationLimit limit;
        limit.types = {IfaceConcurrencyType::STA, IfaceConcurrencyType::AP};
        limit.maxIfaces = 1;
        IWifiChip::ChipConcurrencyCombination combo;
        combo.limits = {limit};
        IWifiChip::ChipMode mode;
        mode.id = 0;
        mode.availableCombinations = {combo};
        *out = {mode};
        return ScopedAStatus::ok();
    }
    ScopedAStatus getFeatureSet(int32_t* out) override {
        *out = 0;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getId(int32_t* out) override {
        *out = 0;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getMode(int32_t* out) override {
        *out = mode_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus registerEventCallback(
            const std::shared_ptr<IWifiChipEventCallback>& cb) override {
        callback_ = cb;
        return ScopedAStatus::ok();
    }
    ScopedAStatus removeStaIface(const std::string& ifname) override {
        std::lock_guard lock(mu_);
        if (sta_ && ifname == "wlan0") sta_.reset();
        return ScopedAStatus::ok();
    }
    ScopedAStatus setCountryCode(const std::array<uint8_t, 2>& code) override {
        country_ = code;
        return ScopedAStatus::ok();
    }

  private:
    std::mutex mu_;
    int32_t mode_ = 0;
    std::array<uint8_t, 2> country_{{'U', 'S'}};
    std::shared_ptr<QemuStaIface> sta_;
    std::shared_ptr<IWifiChipEventCallback> callback_;
};

class QemuWifi : public BnWifi {
  public:
    QemuWifi() : chip_(ndk::SharedRefBase::make<QemuWifiChip>()) {}

    ScopedAStatus getChip(int32_t chipId, std::shared_ptr<IWifiChip>* out) override {
        if (chipId != 0) {
            out->reset();
            return ScopedAStatus::fromServiceSpecificError(
                    static_cast<int32_t>(WifiStatusCode::ERROR_INVALID_ARGS));
        }
        *out = chip_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus getChipIds(std::vector<int32_t>* out) override {
        *out = {0};
        return ScopedAStatus::ok();
    }
    ScopedAStatus isStarted(bool* out) override {
        *out = started_;
        return ScopedAStatus::ok();
    }
    ScopedAStatus registerEventCallback(const std::shared_ptr<IWifiEventCallback>& cb) override {
        callback_ = cb;
        return ScopedAStatus::ok();
    }
    ScopedAStatus start() override {
        if (!started_) {
            started_ = true;
            if (callback_) callback_->onStart();
        }
        return ScopedAStatus::ok();
    }
    ScopedAStatus stop() override {
        if (started_) {
            started_ = false;
            if (callback_) callback_->onStop();
        }
        return ScopedAStatus::ok();
    }

  private:
    bool started_ = false;
    std::shared_ptr<QemuWifiChip> chip_;
    std::shared_ptr<IWifiEventCallback> callback_;
};

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    auto service = ndk::SharedRefBase::make<QemuWifi>();
    constexpr auto instance = "android.hardware.wifi.IWifi/default";
    auto status = AServiceManager_addService(service->asBinder().get(), instance);
    __android_log_print(status == STATUS_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "qemu-wifi",
                        "register %s: %d (simulated chip0/wlan0)", instance, status);
    if (status != STATUS_OK) return 1;
    ABinderProcess_joinThreadPool();
    return 1;
}
