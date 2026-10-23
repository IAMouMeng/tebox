#include "soft_cached_scan.h"

#include <aidl/android/hardware/wifi/CachedScanResult.h>
#include <aidl/android/hardware/wifi/WifiChannelWidthInMhz.h>
#include <aidl/android/hardware/wifi/WifiRatePreamble.h>

#include <array>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace wifi {
namespace {

struct FakeAp {
    std::string ssid;
    std::array<uint8_t, 6> bssid{};
    int32_t freq_mhz = 2412;
    int32_t rssi_dbm = -55;
};

uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

std::vector<FakeAp> gFakeAps;
std::once_flag gOnce;

void initFakeApsOnce() {
    std::call_once(gOnce, [] {
        struct Template {
            const char* fmt;
            unsigned arg_mask;
            uint8_t oui[3];
            int freq;
        };
        const Template templates[] = {
                {"TP-LINK_%04X", 0xffffu, {0x50, 0xC7, 0xBF}, 2412},
                {"TP-LINK_5G_%04X", 0xffffu, {0x50, 0xC7, 0xBF}, 5180},
                {"Xiaomi-%04X", 0xffffu, {0x64, 0xCC, 0x2E}, 2437},
                {"Xiaomi_5G-%04X", 0xffffu, {0x64, 0xCC, 0x2E}, 5745},
                {"ChinaNet-%04X", 0xffffu, {0xD4, 0xEE, 0x07}, 2462},
                {"CMCC-%04X", 0xffffu, {0x00, 0x1E, 0x10}, 2417},
                {"MERCURY_%04X", 0xffffu, {0xD8, 0x15, 0x0D}, 2437},
                {"Tenda_%04X", 0xffffu, {0xC8, 0x3A, 0x35}, 2412},
                {"HUAWEI-%06X", 0xffffffu, {0x48, 0x46, 0xFB}, 5200},
                {"HONOR_5G-%04X", 0xffffu, {0x20, 0xAB, 0x48}, 5220},
                {"ASUS_%04X", 0xffffu, {0x04, 0xD4, 0xC4}, 5785},
                {"NETGEAR%02X", 0xffu, {0xA0, 0x04, 0x60}, 5765},
                {"ChinaUnicom-%04X", 0xffffu, {0xB0, 0x95, 0x8E}, 2462},
                {"Redmi-%04X", 0xffffu, {0x28, 0x6C, 0x07}, 2417},
                {"cu_%04X", 0xffffu, {0x00, 0x1A, 0x2B}, 2437},
                {"DIRECT-%02X-HP DeskJet", 0xffu, {0x98, 0xE7, 0xF4}, 2412},
        };
        auto now = std::chrono::steady_clock::now().time_since_epoch().count();
        uint32_t seed = mix32(static_cast<uint32_t>(now) ^ 0x51a11u);
        gFakeAps.clear();
        for (size_t i = 0; i < sizeof(templates) / sizeof(templates[0]); ++i) {
            seed = mix32(seed + static_cast<uint32_t>(i) * 0x9e3779b9u);
            const Template& t = templates[i];
            FakeAp ap;
            char ssid[64];
            std::snprintf(ssid, sizeof(ssid), t.fmt, seed & t.arg_mask);
            ap.ssid = ssid;
            ap.bssid = {t.oui[0], t.oui[1], t.oui[2],
                        static_cast<uint8_t>((seed >> 16) & 0xff),
                        static_cast<uint8_t>((seed >> 8) & 0xff),
                        static_cast<uint8_t>(seed & 0xff)};
            ap.freq_mhz = t.freq;
            ap.rssi_dbm = -38 - static_cast<int>(seed % 45);
            gFakeAps.push_back(std::move(ap));
        }
    });
}

}  // namespace

CachedScanData makeSoftCachedScanData() {
    initFakeApsOnce();
    CachedScanData data;
    data.scannedFrequenciesMhz = {2412, 2437, 2462, 5180, 5200, 5745};
    auto ts = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();
    for (const auto& ap : gFakeAps) {
        CachedScanResult r;
        r.ssid.assign(ap.ssid.begin(), ap.ssid.end());
        r.bssid = ap.bssid;
        r.rssiDbm = ap.rssi_dbm;
        r.frequencyMhz = ap.freq_mhz;
        r.timeStampInUs = ts;
        r.channelWidthMhz = WifiChannelWidthInMhz::WIDTH_20;
        r.preambleType = WifiRatePreamble::OFDM;
        data.cachedScanResults.push_back(std::move(r));
    }
    return data;
}

}  // namespace wifi
}  // namespace hardware
}  // namespace android
}  // namespace aidl
