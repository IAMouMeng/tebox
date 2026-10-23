/*
 * Soft legacy WiFi HAL for QEMU GSI: dummy wlan0 + fixed fake scan catalog.
 * Pattern mirrors aosp_arm64-BP4A / OriginOS soft WiFi stubs, hooked into the
 * Google AIDL wifi service via installSoftLegacyHalHooks().
 */

#include "soft_legacy_hal.h"

#include <android-base/logging.h>

#include <algorithm>
#include <algorithm>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace aidl {
namespace android {
namespace hardware {
namespace wifi {
namespace legacy_hal {
namespace {

constexpr const char* kIfaceName = "wlan0";

struct SoftHalState {
    std::mutex mu;
    std::condition_variable cv;
    bool running = false;
    bool stop_requested = false;
    wifi_cleaned_up_handler cleanup_handler = nullptr;
    // Opaque handles — addresses of these members are passed as wifi_*_handle.
    int global_sentinel = 1;
    int iface_sentinel = 1;
    wifi_interface_handle iface_list[1]{};
};

SoftHalState gSoft;

uint32_t mix32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

struct FakeAp {
    std::string ssid;
    std::array<uint8_t, 6> bssid{};
    int32_t freq_mhz = 2412;
    int8_t rssi_dbm = -55;
};

std::vector<FakeAp> gFakeAps;
std::once_flag gFakeOnce;

void ensureFakeAps() {
    std::call_once(gFakeOnce, [] {
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
        gFakeAps.reserve(sizeof(templates) / sizeof(templates[0]));
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
            ap.rssi_dbm = static_cast<int8_t>(-38 - static_cast<int>(seed % 45));
            gFakeAps.push_back(std::move(ap));
        }
        LOG(INFO) << "soft wifi: fixed fake SSID catalog size=" << gFakeAps.size();
    });
}

void ensureDummyWlan0() {
    // Best-effort: SetWifiUpState / wificond need a real netdev named wlan0.
    (void)std::system("ip link add wlan0 type dummy 2>/dev/null");
    (void)std::system("ip link set wlan0 address 02:00:00:00:00:01 up 2>/dev/null");
}

wifi_error soft_wait_for_driver_ready() {
    ensureDummyWlan0();
    return WIFI_SUCCESS;
}

wifi_error soft_initialize(wifi_handle* handle) {
    if (!handle) return WIFI_ERROR_INVALID_ARGS;
    ensureDummyWlan0();
    ensureFakeAps();
    std::lock_guard<std::mutex> lock(gSoft.mu);
    gSoft.stop_requested = false;
    gSoft.running = true;
    gSoft.iface_list[0] = reinterpret_cast<wifi_interface_handle>(&gSoft.iface_sentinel);
    *handle = reinterpret_cast<wifi_handle>(&gSoft.global_sentinel);
    LOG(INFO) << "soft wifi: wifi_initialize -> wlan0";
    return WIFI_SUCCESS;
}

void soft_cleanup(wifi_handle handle, wifi_cleaned_up_handler handler) {
    {
        std::lock_guard<std::mutex> lock(gSoft.mu);
        gSoft.cleanup_handler = handler;
        gSoft.stop_requested = true;
    }
    gSoft.cv.notify_all();
    // Invoke cleanup handler synchronously after event loop wakes; if the loop
    // is not running, call immediately.
    bool need_immediate = false;
    {
        std::lock_guard<std::mutex> lock(gSoft.mu);
        if (!gSoft.running) need_immediate = true;
    }
    if (need_immediate && handler) {
        handler(handle);
    }
}

void soft_event_loop(wifi_handle handle) {
    std::unique_lock<std::mutex> lock(gSoft.mu);
    gSoft.running = true;
    gSoft.cv.wait(lock, [] { return gSoft.stop_requested; });
    wifi_cleaned_up_handler handler = gSoft.cleanup_handler;
    gSoft.cleanup_handler = nullptr;
    gSoft.running = false;
    lock.unlock();
    if (handler) {
        handler(handle);
    }
}

wifi_error soft_get_ifaces(wifi_handle /*handle*/, int* num, wifi_interface_handle** ifaces) {
    if (!num || !ifaces) return WIFI_ERROR_INVALID_ARGS;
    gSoft.iface_list[0] = reinterpret_cast<wifi_interface_handle>(&gSoft.iface_sentinel);
    *num = 1;
    *ifaces = gSoft.iface_list;
    return WIFI_SUCCESS;
}

wifi_error soft_get_iface_name(wifi_interface_handle /*iface*/, char* name, size_t size) {
    if (!name || size == 0) return WIFI_ERROR_INVALID_ARGS;
    std::snprintf(name, size, "%s", kIfaceName);
    return WIFI_SUCCESS;
}

wifi_error soft_virtual_interface_create(wifi_handle /*handle*/, const char* ifname,
                                         wifi_interface_type /*type*/) {
    ensureDummyWlan0();
    LOG(INFO) << "soft wifi: virtual_interface_create " << (ifname ? ifname : "(null)");
    return WIFI_SUCCESS;
}

wifi_error soft_virtual_interface_delete(wifi_handle /*handle*/, const char* ifname) {
    LOG(INFO) << "soft wifi: virtual_interface_delete " << (ifname ? ifname : "(null)");
    return WIFI_SUCCESS;
}

wifi_error soft_get_supported_feature_set(wifi_interface_handle /*iface*/, feature_set* set) {
    if (!set) return WIFI_ERROR_INVALID_ARGS;
    *set = WIFI_FEATURE_INFRA | WIFI_FEATURE_INFRA_5G | WIFI_FEATURE_SCAN_RAND |
           WIFI_FEATURE_CACHED_SCAN_RESULTS;
    return WIFI_SUCCESS;
}

wifi_error soft_get_chip_feature_set(wifi_handle /*handle*/, feature_set* set) {
    if (!set) return WIFI_ERROR_INVALID_ARGS;
    *set = WIFI_FEATURE_INFRA | WIFI_FEATURE_INFRA_5G | WIFI_FEATURE_SCAN_RAND |
           WIFI_FEATURE_CACHED_SCAN_RESULTS;
    return WIFI_SUCCESS;
}

wifi_error soft_get_cached_scan_results(wifi_interface_handle /*iface*/,
                                        wifi_cached_scan_result_handler handler) {
    ensureFakeAps();
    if (!handler.on_cached_scan_results) {
        return WIFI_ERROR_INVALID_ARGS;
    }

    static thread_local std::vector<wifi_cached_scan_result> results;
    static thread_local std::vector<u32> freqs;
    results.clear();
    freqs = {2412, 2437, 2462, 5180, 5200, 5745};

    auto ts = std::chrono::duration_cast<std::chrono::microseconds>(
                      std::chrono::steady_clock::now().time_since_epoch())
                      .count();

    results.reserve(gFakeAps.size());
    for (const auto& ap : gFakeAps) {
        wifi_cached_scan_result r{};
        r.age_ms = 0;
        r.capability = 0x0411;  // ESS | Privacy
        r.ssid_len = static_cast<u8>(std::min(ap.ssid.size(), sizeof(r.ssid) - 1));
        std::memcpy(r.ssid, ap.ssid.data(), r.ssid_len);
        r.ssid[r.ssid_len] = 0;
        std::memcpy(r.bssid, ap.bssid.data(), 6);
        r.flags = WIFI_CACHED_SCAN_RESULT_FLAGS_NONE;
        r.rssi = ap.rssi_dbm;
        r.chanspec.width = WIFI_CHAN_WIDTH_20;
        r.chanspec.primary_frequency = ap.freq_mhz;
        r.chanspec.center_frequency0 = ap.freq_mhz;
        r.chanspec.center_frequency1 = 0;
        results.push_back(r);
    }

    wifi_cached_scan_report report{};
    report.ts = static_cast<wifi_timestamp>(ts);
    report.scanned_freq_num = static_cast<u16>(freqs.size());
    report.scanned_freq_list = freqs.data();
    report.result_cnt = static_cast<u8>(std::min(results.size(), size_t{255}));
    report.results = results.data();
    handler.on_cached_scan_results(&report);
    return WIFI_SUCCESS;
}

wifi_error soft_early_initialize() {
    ensureDummyWlan0();
    return WIFI_SUCCESS;
}

}  // namespace

void installSoftLegacyHalHooks(wifi_hal_fn* hal_fn) {
    if (!hal_fn) return;
    hal_fn->wifi_wait_for_driver_ready = soft_wait_for_driver_ready;
    hal_fn->wifi_initialize = soft_initialize;
    hal_fn->wifi_cleanup = soft_cleanup;
    hal_fn->wifi_event_loop = soft_event_loop;
    hal_fn->wifi_get_ifaces = soft_get_ifaces;
    hal_fn->wifi_get_iface_name = soft_get_iface_name;
    hal_fn->wifi_virtual_interface_create = soft_virtual_interface_create;
    hal_fn->wifi_virtual_interface_delete = soft_virtual_interface_delete;
    hal_fn->wifi_get_supported_feature_set = soft_get_supported_feature_set;
    hal_fn->wifi_get_chip_feature_set = soft_get_chip_feature_set;
    hal_fn->wifi_get_cached_scan_results = soft_get_cached_scan_results;
    hal_fn->wifi_early_initialize = soft_early_initialize;
    LOG(INFO) << "soft wifi: legacy HAL hooks installed";
}

}  // namespace legacy_hal
}  // namespace wifi
}  // namespace hardware
}  // namespace android
}  // namespace aidl
