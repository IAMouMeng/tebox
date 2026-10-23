/*
 * Copyright 2024 The Android Open Source Project
 *
 * C++ NDK port of CVD guest/hals/bluetooth/src/hci.rs (android-16.0.0_r4).
 * IBluetoothHci/default over H4 UART at /dev/hvc5. This QEMU image has no
 * virtio-console HCI / rootcanal, so initialize() falls back to an in-process
 * software controller instead of creating a host device.
 */
#pragma once

#include <aidl/android/hardware/bluetooth/BnBluetoothHci.h>
#include <aidl/android/hardware/bluetooth/IBluetoothHciCallbacks.h>

#include <mutex>
#include <string>

#include "async_fd_watcher.h"
#include "h4_protocol.h"

namespace aidl::android::hardware::bluetooth::impl {

class BluetoothHci : public BnBluetoothHci {
 public:
  BluetoothHci(const std::string& dev_path = "/dev/hvc5");

  ndk::ScopedAStatus initialize(
      const std::shared_ptr<IBluetoothHciCallbacks>& cb) override;
  ndk::ScopedAStatus sendHciCommand(const std::vector<uint8_t>& packet) override;
  ndk::ScopedAStatus sendAclData(const std::vector<uint8_t>& packet) override;
  ndk::ScopedAStatus sendScoData(const std::vector<uint8_t>& packet) override;
  ndk::ScopedAStatus sendIsoData(const std::vector<uint8_t>& packet) override;
  ndk::ScopedAStatus close() override;

 private:
  int mFd{-1};
  std::shared_ptr<IBluetoothHciCallbacks> mCb;
  std::shared_ptr<::android::hardware::bluetooth::hci::H4Protocol> mH4;
  std::string mDevPath;
  ::android::hardware::bluetooth::async::AsyncFdWatcher mFdWatcher;
  bool mSoftware{false};

  std::mutex mStateMutex;
  enum class HalState { Closed, Opened } mState{HalState::Closed};

  int openSerial();
  void drain(int fd);
  bool sendResetAndWait();
  void send(::android::hardware::bluetooth::hci::PacketType type,
            const std::vector<uint8_t>& packet);
  void sendSoftware(const std::vector<uint8_t>& command);
};

}  // namespace aidl::android::hardware::bluetooth::impl
