#pragma once

namespace android::wifi_hal {

class DriverTool {
  public:
    static constexpr int kFirmwareModeSta = 0;
    static constexpr int kFirmwareModeAp = 1;
    static constexpr int kFirmwareModeP2p = 2;

    bool LoadDriver() { return true; }
    bool UnloadDriver() { return true; }
    bool IsFirmwareModeChangeNeeded(int) { return false; }
    bool ChangeFirmwareMode(int) { return true; }
};

}  // namespace android::wifi_hal
