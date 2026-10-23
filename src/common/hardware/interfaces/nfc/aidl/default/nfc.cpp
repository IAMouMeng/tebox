// C++ NDK port of CVD guest/hals/nfc/src/nfc.rs (android-16.0.0_r4).
// INfc AIDL v2 + virtio-console NCI. If no console is present, open/write
// still complete so libnfc-nci does not hang.
#include <aidl/android/hardware/nfc/BnNfc.h>
#include <aidl/android/hardware/nfc/INfcClientCallback.h>
#include <aidl/android/hardware/nfc/NfcCloseType.h>
#include <aidl/android/hardware/nfc/NfcConfig.h>
#include <aidl/android/hardware/nfc/NfcEvent.h>
#include <aidl/android/hardware/nfc/NfcStatus.h>
#include <aidl/android/hardware/nfc/PresenceCheckAlgorithm.h>
#include <aidl/android/hardware/nfc/ProtocolDiscoveryConfig.h>
#include <android/binder_ibinder.h>
#include <android/binder_manager.h>
#include <android/binder_process.h>
#include <android/log.h>

#include <cerrno>
#include <dirent.h>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>

#include <atomic>
#include <fstream>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace aidl::android::hardware::nfc;
using ndk::ScopedAStatus;

namespace {

constexpr const char* kTag = "CfNfc";
constexpr size_t kBufSize = 1024;
constexpr size_t kNciHeaderSize = 3;

int openVirtioConsole() {
    const char* env = getenv("TEBOX_NFC_VIRTIO");
    if (env && env[0]) {
        int fd = ::open(env, O_RDWR | O_CLOEXEC);
        if (fd >= 0) return fd;
    }
    DIR* dir = opendir("/sys/class/virtio-ports");
    if (!dir) return -1;
    int fd = -1;
    while (dirent* e = readdir(dir)) {
        if (e->d_name[0] == '.') continue;
        std::ifstream f(std::string("/sys/class/virtio-ports/") + e->d_name + "/name");
        std::string n;
        if (!std::getline(f, n)) continue;
        if (n == "nfc" || n == "tebox.nfc" || n.find("nfc") != std::string::npos) {
            fd = ::open((std::string("/dev/") + e->d_name).c_str(), O_RDWR | O_CLOEXEC);
            if (fd >= 0) {
                __android_log_print(ANDROID_LOG_INFO, kTag, "virtio-console %s on /dev/%s",
                                    n.c_str(), e->d_name);
                break;
            }
        }
    }
    closedir(dir);
    return fd;
}

void setRaw(int fd) {
    termios attrs{};
    if (tcgetattr(fd, &attrs) != 0) return;
    cfmakeraw(&attrs);
    tcsetattr(fd, TCSANOW, &attrs);
}

class CvdNfc final : public BnNfc {
  public:
    CvdNfc() {
        fd_ = openVirtioConsole();
        if (fd_ >= 0) {
            setRaw(fd_);
            reader_ = std::thread([this]() { readerLoop(); });
        } else {
            __android_log_print(ANDROID_LOG_INFO, kTag,
                                "no virtio-console; INfc open/write still complete");
        }
    }

    ~CvdNfc() override {
        stop_ = true;
        if (fd_ >= 0) {
            int fd = fd_;
            fd_ = -1;
            ::close(fd);
        }
        if (reader_.joinable()) reader_.join();
        unlinkDeathLocked();
    }

    ScopedAStatus open(const std::shared_ptr<INfcClientCallback>& callback) override {
        __android_log_print(ANDROID_LOG_INFO, kTag, "open");
        std::lock_guard lock(mu_);
        if (cb_) {
            __android_log_print(ANDROID_LOG_INFO, kTag, "already opened. closing first.");
            sendEventLocked(NfcEvent::CLOSE_CPLT, NfcStatus::OK);
            unlinkDeathLocked();
            cb_.reset();
        }
        cb_ = callback;
        if (cb_) {
            death_ = AIBinder_DeathRecipient_new(&CvdNfc::onDied);
            AIBinder_linkToDeath(cb_->asBinder().get(), death_, this);
        }
        sendEventLocked(NfcEvent::OPEN_CPLT, NfcStatus::OK);
        return ScopedAStatus::ok();
    }

    ScopedAStatus close(NfcCloseType /*type*/) override {
        __android_log_print(ANDROID_LOG_INFO, kTag, "close");
        std::lock_guard lock(mu_);
        if (!cb_) {
            return ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(NfcStatus::FAILED));
        }
        sendEventLocked(NfcEvent::CLOSE_CPLT, NfcStatus::OK);
        unlinkDeathLocked();
        cb_.reset();
        return ScopedAStatus::ok();
    }

    ScopedAStatus coreInitialized() override {
        __android_log_print(ANDROID_LOG_INFO, kTag, "coreInitialized");
        std::lock_guard lock(mu_);
        if (!cb_) {
            return ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(NfcStatus::FAILED));
        }
        sendEventLocked(NfcEvent::POST_INIT_CPLT, NfcStatus::OK);
        return ScopedAStatus::ok();
    }

    ScopedAStatus factoryReset() override {
        __android_log_print(ANDROID_LOG_INFO, kTag, "factoryReset");
        return ScopedAStatus::ok();
    }

    ScopedAStatus getConfig(NfcConfig* out) override {
        __android_log_print(ANDROID_LOG_INFO, kTag, "getConfig");
        NfcConfig cfg{};
        cfg.nfaPollBailOutMode = true;
        cfg.maxIsoDepTransceiveLength = 0xFEFF;
        cfg.defaultOffHostRoute = static_cast<int8_t>(0x81);
        cfg.defaultOffHostRouteFelica = static_cast<int8_t>(0x81);
        cfg.defaultSystemCodeRoute = 0x00;
        cfg.defaultSystemCodePowerState = 0x3B;
        cfg.defaultRoute = 0x00;
        cfg.offHostRouteUicc = {static_cast<uint8_t>(0x81)};
        cfg.offHostRouteEse = {static_cast<uint8_t>(0x81)};
        cfg.defaultIsoDepRoute = static_cast<int8_t>(0x81);
        cfg.presenceCheckAlgorithm = PresenceCheckAlgorithm::DEFAULT;
        cfg.nfaProprietaryCfg = ProtocolDiscoveryConfig{};
        *out = cfg;
        return ScopedAStatus::ok();
    }

    ScopedAStatus powerCycle() override {
        __android_log_print(ANDROID_LOG_INFO, kTag, "powerCycle");
        std::lock_guard lock(mu_);
        if (!cb_) {
            return ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(NfcStatus::FAILED));
        }
        sendEventLocked(NfcEvent::OPEN_CPLT, NfcStatus::OK);
        return ScopedAStatus::ok();
    }

    ScopedAStatus preDiscover() override {
        __android_log_print(ANDROID_LOG_INFO, kTag, "preDiscover");
        std::lock_guard lock(mu_);
        if (!cb_) {
            return ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(NfcStatus::FAILED));
        }
        sendEventLocked(NfcEvent::PRE_DISCOVER_CPLT, NfcStatus::OK);
        return ScopedAStatus::ok();
    }

    ScopedAStatus write(const std::vector<uint8_t>& data, int32_t* out) override {
        __android_log_print(ANDROID_LOG_INFO, kTag, "write, sz=%zu", data.size());
        std::lock_guard lock(mu_);
        if (!cb_) {
            return ScopedAStatus::fromServiceSpecificError(static_cast<int32_t>(NfcStatus::FAILED));
        }
        if (fd_ >= 0 && !data.empty()) {
            const uint8_t* p = data.data();
            size_t left = data.size();
            while (left > 0) {
                ssize_t n = ::write(fd_, p, left);
                if (n > 0) {
                    p += n;
                    left -= static_cast<size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                break;
            }
        }
        *out = static_cast<int32_t>(data.size());
        return ScopedAStatus::ok();
    }

    ScopedAStatus setEnableVerboseLogging(bool enable) override {
        verbose_ = enable;
        return ScopedAStatus::ok();
    }

    ScopedAStatus isVerboseLoggingEnabled(bool* out) override {
        *out = verbose_;
        return ScopedAStatus::ok();
    }

    ScopedAStatus controlGranted(NfcStatus* out) override {
        *out = NfcStatus::OK;
        return ScopedAStatus::ok();
    }

    void onClientDied() {
        std::lock_guard lock(mu_);
        __android_log_print(ANDROID_LOG_INFO, kTag, "Nfc service has died");
        cb_.reset();
        if (death_) {
            AIBinder_DeathRecipient_delete(death_);
            death_ = nullptr;
        }
    }

  private:
    static void onDied(void* cookie) { static_cast<CvdNfc*>(cookie)->onClientDied(); }

    void sendEventLocked(NfcEvent event, NfcStatus status) {
        if (cb_) cb_->sendEvent(event, status);
    }

    void unlinkDeathLocked() {
        if (cb_ && death_) {
            AIBinder_unlinkToDeath(cb_->asBinder().get(), death_, this);
        }
        if (death_) {
            AIBinder_DeathRecipient_delete(death_);
            death_ = nullptr;
        }
    }

    void readerLoop() {
        std::vector<uint8_t> buf(kBufSize);
        while (!stop_) {
            int fd = fd_;
            if (fd < 0) break;
            ssize_t n = ::read(fd, buf.data(), kNciHeaderSize);
            if (n < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (n == 0) {
                usleep(20000);
                continue;
            }
            if (static_cast<size_t>(n) < kNciHeaderSize) continue;
            size_t total = static_cast<size_t>(buf[2]) + kNciHeaderSize;
            if (total > buf.size()) total = buf.size();
            size_t got = kNciHeaderSize;
            while (got < total) {
                n = ::read(fd, buf.data() + got, total - got);
                if (n > 0) {
                    got += static_cast<size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                break;
            }
            std::shared_ptr<INfcClientCallback> cb;
            {
                std::lock_guard lock(mu_);
                cb = cb_;
            }
            if (cb) {
                cb->sendData(std::vector<uint8_t>(buf.begin(), buf.begin() + got));
            }
        }
    }

    std::mutex mu_;
    std::shared_ptr<INfcClientCallback> cb_;
    AIBinder_DeathRecipient* death_ = nullptr;
    int fd_ = -1;
    std::thread reader_;
    std::atomic<bool> stop_{false};
    bool verbose_ = false;
};

}  // namespace

int main() {
    ABinderProcess_setThreadPoolMaxThreadCount(4);
    ABinderProcess_startThreadPool();
    auto service = ndk::SharedRefBase::make<CvdNfc>();
    constexpr auto instance = "android.hardware.nfc.INfc/default";
    auto status = AServiceManager_addService(service->asBinder().get(), instance);
    __android_log_print(status == STATUS_OK ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, kTag,
                        "Starting %s", instance);
    if (status != STATUS_OK) return 1;
    ABinderProcess_joinThreadPool();
    return 1;
}
