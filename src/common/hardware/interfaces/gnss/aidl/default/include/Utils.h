#ifndef android_hardware_gnss_common_default_Utils_H_
#define android_hardware_gnss_common_default_Utils_H_

#include <aidl/android/hardware/gnss/BnGnss.h>
#include <aidl/android/hardware/gnss/BnGnssMeasurementInterface.h>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace android {
namespace hardware {
namespace gnss {
namespace common {

struct Utils {
    static aidl::android::hardware::gnss::GnssData getMockMeasurement(
            const bool enableCorrVecOutputs, const bool enableFullTracking);
    static aidl::android::hardware::gnss::GnssLocation getMockLocation();
    static std::vector<aidl::android::hardware::gnss::IGnssCallback::GnssSvInfo>
    getMockSvInfoList();
};

struct ThreadBlocker {
    template <class R, class P>
    bool wait_for(std::chrono::duration<R, P> const& time) {
        std::unique_lock<std::mutex> lock(m);
        terminate = false;
        return !cv.wait_for(lock, time, [&] { return terminate; });
    }

    void wait() {
        std::unique_lock<std::mutex> lock(m);
        terminate = false;
        cv.wait(lock, [&] { return terminate; });
    }

    void notify() {
        std::unique_lock<std::mutex> lock(m);
        terminate = true;
        cv.notify_all();
    }

    void reset() {
        std::unique_lock<std::mutex> lock(m);
        terminate = false;
    }

  private:
    std::condition_variable cv;
    std::mutex m;
    bool terminate = false;
};

}  // namespace common
}  // namespace gnss
}  // namespace hardware
}  // namespace android

#endif
