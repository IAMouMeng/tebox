/*
 * Copyright 2024 The Android Open Source Project
 *
 * C++ NDK port of CVD guest/hals/bluetooth/src/hci.rs (android-16.0.0_r4).
 */

#define LOG_TAG "bluetooth-cf"

#include "BluetoothHci.h"

#include <android/binder_auto_utils.h>

#include <chrono>
#include <future>

#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <cutils/properties.h>
#include <log/log.h>

using namespace ::android::hardware::bluetooth::hci;
using namespace ::android::hardware::bluetooth::async;
using aidl::android::hardware::bluetooth::IBluetoothHciCallbacks;
using aidl::android::hardware::bluetooth::Status;

namespace aidl::android::hardware::bluetooth::impl {

namespace {
int SetTerminalRaw(int fd) {
  termios terminal_settings;
  if (tcgetattr(fd, &terminal_settings) < 0) return -1;
  cfmakeraw(&terminal_settings);
  return tcsetattr(fd, TCSANOW, &terminal_settings);
}
}  // namespace

BluetoothHci::BluetoothHci(const std::string& dev_path) {
  char property_bytes[PROPERTY_VALUE_MAX];
  property_get("vendor.ser.bt-uart", property_bytes, dev_path.c_str());
  mDevPath = std::string(property_bytes);
}

void BluetoothHci::drain(int fd) {
  int flags = fcntl(fd, F_GETFL, 0);
  if (flags < 0) return;
  fcntl(fd, F_SETFL, flags | O_NONBLOCK);
  uint8_t data[4096];
  for (;;) {
    ssize_t n = TEMP_FAILURE_RETRY(read(fd, data, sizeof(data)));
    if (n == 0) break;
    if (n < 0) {
      if (errno != EAGAIN && errno != EWOULDBLOCK) {
        ALOGW("drain %s: %s", mDevPath.c_str(), strerror(errno));
      }
      break;
    }
    if (static_cast<size_t>(n) < sizeof(data)) break;
  }
  fcntl(fd, F_SETFL, flags);
}

int BluetoothHci::openSerial() {
  int fd = open(mDevPath.c_str(), O_RDWR | O_CLOEXEC);
  if (fd < 0) {
    ALOGI("no HCI serial %s (%s); using in-process software controller",
          mDevPath.c_str(), strerror(errno));
    return fd;
  }
  if (SetTerminalRaw(fd) < 0) {
    ALOGI("Could not make %s a raw terminal (%s)", mDevPath.c_str(),
          strerror(errno));
  }
  drain(fd);
  return fd;
}

bool BluetoothHci::sendResetAndWait() {
  auto resetPromise = std::make_shared<std::promise<void>>();
  auto resetFuture = resetPromise->get_future();
  mH4 = std::make_shared<H4Protocol>(
      mFd,
      [](const std::vector<uint8_t>&) {},
      [](const std::vector<uint8_t>&) {},
      [](const std::vector<uint8_t>&) {},
      [resetPromise](const std::vector<uint8_t>& raw_event) {
        // HCI Reset Complete: 0e 04 01 03 0c 00 (ignore Num_HCI_Command_Packets).
        if (raw_event.size() == 6 && raw_event[0] == 0x0e && raw_event[1] == 0x04 &&
            raw_event[3] == 0x03 && raw_event[4] == 0x0c && raw_event[5] == 0x00) {
          resetPromise->set_value();
        }
      },
      [](const std::vector<uint8_t>&) {},
      [this]() { mFdWatcher.StopWatchingFileDescriptors(); });
  mFdWatcher.WatchFdForNonBlockingReads(mFd, [this](int) { mH4->OnDataReady(); });
  send(PacketType::COMMAND, {0x03, 0x0c, 0x00});
  auto status = resetFuture.wait_for(std::chrono::seconds(1));
  mFdWatcher.StopWatchingFileDescriptors();
  mH4.reset();
  if (status != std::future_status::ready) {
    ALOGE("HCI Reset Complete not received");
    return false;
  }
  ALOGI("HCI Reset successful");
  return true;
}

ndk::ScopedAStatus BluetoothHci::initialize(
    const std::shared_ptr<IBluetoothHciCallbacks>& cb) {
  ALOGI("IBluetoothHci::initialize");
  if (cb == nullptr) {
    ALOGE("cb == nullptr");
    return ndk::ScopedAStatus::fromExceptionCode(EX_ILLEGAL_ARGUMENT);
  }

  // Stack often restarts without close() (crash / disable). Reporting
  // ALREADY_INITIALIZED makes libbluetooth CHECK-fail and crash-loop.
  {
    std::lock_guard<std::mutex> guard(mStateMutex);
    if (mState != HalState::Closed) {
      ALOGW("IBluetoothHci::initialize: stale Opened state; resetting");
      mFdWatcher.StopWatchingFileDescriptors();
      if (mFd >= 0) {
        ::close(mFd);
        mFd = -1;
      }
      mH4.reset();
      mSoftware = false;
      mCb.reset();
      mState = HalState::Closed;
    }
    mState = HalState::Opened;
  }

  mCb = cb;
  // QEMU virtio-console nodes (/dev/hvc*) exist but are not HCI UARTs.
  // Only open a serial when the board explicitly points vendor.ser.bt-uart
  // at a real HCI device; otherwise use the in-process software controller.
  char uart_prop[PROPERTY_VALUE_MAX] = {};
  property_get("vendor.ser.bt-uart", uart_prop, "");
  if (uart_prop[0] != '\0') {
    mFd = openSerial();
  } else {
    ALOGI("vendor.ser.bt-uart unset; using in-process software controller");
    mFd = -1;
  }
  if (mFd < 0) {
    mSoftware = true;
    mCb->initializationComplete(Status::SUCCESS);
    return ndk::ScopedAStatus::ok();
  }

  // CVD: HAL itself sends HCI Reset and reports initializationComplete
  // only after Reset Complete, so leftover controller state is cleared.
  if (!sendResetAndWait()) {
    ::close(mFd);
    mFd = -1;
    mSoftware = true;
    ALOGI("serial HCI Reset failed; using in-process software controller");
    mCb->initializationComplete(Status::SUCCESS);
    return ndk::ScopedAStatus::ok();
  }

  mH4 = std::make_shared<H4Protocol>(
      mFd,
      [](const std::vector<uint8_t>&) { ALOGE("Unexpected command from controller"); },
      [this](const std::vector<uint8_t>& raw_acl) { mCb->aclDataReceived(raw_acl); },
      [this](const std::vector<uint8_t>& raw_sco) { mCb->scoDataReceived(raw_sco); },
      [this](const std::vector<uint8_t>& raw_event) { mCb->hciEventReceived(raw_event); },
      [this](const std::vector<uint8_t>& raw_iso) { mCb->isoDataReceived(raw_iso); },
      [this]() {
        ALOGI("HCI serial disconnected");
        mFdWatcher.StopWatchingFileDescriptors();
      });
  mFdWatcher.WatchFdForNonBlockingReads(mFd, [this](int) { mH4->OnDataReady(); });
  mCb->initializationComplete(Status::SUCCESS);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothHci::close() {
  ALOGI("IBluetoothHci::close");
  std::lock_guard<std::mutex> guard(mStateMutex);
  mState = HalState::Closed;
  mFdWatcher.StopWatchingFileDescriptors();
  if (mFd >= 0) {
    ::close(mFd);
    mFd = -1;
  }
  mH4.reset();
  mSoftware = false;
  mCb.reset();
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothHci::sendHciCommand(const std::vector<uint8_t>& packet) {
  if (mSoftware) {
    sendSoftware(packet);
    return ndk::ScopedAStatus::ok();
  }
  send(PacketType::COMMAND, packet);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothHci::sendAclData(const std::vector<uint8_t>& packet) {
  if (!mSoftware) send(PacketType::ACL_DATA, packet);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothHci::sendScoData(const std::vector<uint8_t>& packet) {
  if (!mSoftware) send(PacketType::SCO_DATA, packet);
  return ndk::ScopedAStatus::ok();
}

ndk::ScopedAStatus BluetoothHci::sendIsoData(const std::vector<uint8_t>& packet) {
  if (!mSoftware) send(PacketType::ISO_DATA, packet);
  return ndk::ScopedAStatus::ok();
}

void BluetoothHci::send(PacketType type, const std::vector<uint8_t>& v) {
  if (v.empty()) {
    ALOGE("IBluetoothHci::sendXX: empty packet");
    return;
  }
  std::lock_guard<std::mutex> guard(mStateMutex);
  if (mH4 == nullptr) {
    ALOGE("IBluetoothHci::sendXX: not initialized");
    return;
  }
  mH4->Send(type, v);
}

void BluetoothHci::sendSoftware(const std::vector<uint8_t>& cmd) {
  if (!mCb || cmd.size() < 2) return;
  const uint16_t op = static_cast<uint16_t>(cmd[0] | (cmd[1] << 8));
  auto complete = [&](const std::vector<uint8_t>& ret) {
    std::vector<uint8_t> ev;
    ev.push_back(0x0e);
    ev.push_back(static_cast<uint8_t>(3 + ret.size()));
    ev.push_back(0x01);
    ev.push_back(static_cast<uint8_t>(op & 0xff));
    ev.push_back(static_cast<uint8_t>((op >> 8) & 0xff));
    ev.insert(ev.end(), ret.begin(), ret.end());
    mCb->hciEventReceived(ev);
  };
  auto status = [&](uint8_t st) {
    mCb->hciEventReceived({0x0f, 0x04, st, 0x01, static_cast<uint8_t>(op & 0xff),
                           static_cast<uint8_t>((op >> 8) & 0xff)});
  };
  // Connection / SCO / ISO commands use Command Status. Succeeding them (or
  // answering Command Complete) makes the stack think an A2DP/SCO peer exists
  // and AudioPolicy routes PCM off the speaker. Disallow them so media stays
  // on IModule/default → tebox.audio_out.
  switch (op) {
    case 0x0405:  // Create Connection
    case 0x0406:  // Disconnect
    case 0x0407:  // Add SCO Connection
    case 0x0409:  // Accept Connection Request
    case 0x040a:  // Reject Connection Request
    case 0x040f:  // Change Connection Packet Type
    case 0x0411:  // Authentication Requested
    case 0x0413:  // Set Connection Encryption
    case 0x0419:  // Remote Name Request
    case 0x041b:  // Read Remote Supported Features
    case 0x041c:  // Read Remote Extended Features
    case 0x041d:  // Read Remote Version Information
    case 0x0428:  // Setup Synchronous Connection
    case 0x0429:  // Accept Synchronous Connection
    case 0x042a:  // Reject Synchronous Connection
    case 0x043d:  // Enhanced Setup Synchronous Connection
    case 0x043e:  // Enhanced Accept Synchronous Connection
    case 0x200d:  // LE Create Connection
    case 0x200e:  // LE Create Connection Cancel
    case 0x2013:  // LE Connection Update
    case 0x2016:  // LE Read Remote Features
    case 0x2019:  // LE Start Encryption
    case 0x2043:  // LE Extended Create Connection
    case 0x2064:  // LE Create CIS
    case 0x2065:  // LE Accept CIS Request
    case 0x2066:  // LE Reject CIS Request
    case 0x2068:  // LE Create BIG
      status(0x0c);  // Command Disallowed
      return;
    default:
      break;
  }
  switch (op) {
    case 0x0c03:  // Reset
      complete({0x00});
      break;
    case 0x0c13:  // Write Local Name
      complete({0x00});
      break;
    case 0x0c14: {  // Read Local Name: Status + 248-byte null-padded name
      std::vector<uint8_t> ret(1 + 248, 0x00);
      const char name[] = "GKI Soft BT";
      for (size_t i = 0; name[i] != '\0' && i < 248; ++i) ret[1 + i] = static_cast<uint8_t>(name[i]);
      complete(ret);
      break;
    }
    case 0x0c15:  // Read Connection Accept Timeout
      complete({0x00, 0x00, 0x20});
      break;
    case 0x0c18:  // Read Page Timeout
      complete({0x00, 0x00, 0x20});
      break;
    case 0x0c19:  // Read Scan Enable
      complete({0x00, 0x00});
      break;
    case 0x0c1b:  // Read Page Scan Activity
      complete({0x00, 0x00, 0x08, 0x12, 0x00});
      break;
    case 0x0c1d:  // Read Inquiry Scan Activity
      complete({0x00, 0x00, 0x08, 0x12, 0x00});
      break;
    case 0x0c23:  // Read Class of Device
      complete({0x00, 0x00, 0x1f, 0x00});
      break;
    case 0x0c25:  // Read Voice Setting
      complete({0x00, 0x00, 0x00});
      break;
    case 0x0c38:  // Read Number Of Supported IAC
      complete({0x00, 0x01});
      break;
    case 0x0c39:  // Read Current IAC LAP
      complete({0x00, 0x01, 0x33, 0x8b, 0x9e});
      break;
    case 0x0c45:  // Read Inquiry Mode
      complete({0x00, 0x00});
      break;
    case 0x0c51:  // Read Extended Inquiry Response
      complete(std::vector<uint8_t>(1 + 240, 0x00));
      break;
    case 0x0c55:  // Read Simple Pairing Mode
      complete({0x00, 0x01});
      break;
    case 0x0c58:  // Read Inquiry Response TX Power Level
      complete({0x00, 0x00});
      break;
    case 0x0c5a:  // Read Default Erroneous Data Reporting
      complete({0x00, 0x00});
      break;
    case 0x0c63:  // Enhanced Flush (Command Status style elsewhere); Complete OK
      complete({0x00});
      break;
    case 0x0c69:  // Read LE Host Support
      complete({0x00, 0x01, 0x00});
      break;
    case 0x1001:  // Read Local Version Information
      complete({0x00, 0x0d, 0x00, 0x00, 0x0d, 0x75, 0x00, 0x0d, 0x00});
      break;
    case 0x1002: {  // Read Local Supported Commands: no Create Connection / SCO.
      std::vector<uint8_t> ret(65, 0x00);
      ret[0] = 0x00;
      ret[1] = 0x23;  // Inquiry, Inquiry Cancel, Disconnect
      ret[4] = 0xc0;  // Read / Write Scan Enable
      ret[5] = 0x03;  // Read / Write Page Scan Activity
      complete(ret);
      break;
    }
    case 0x1003:  // Read Local Supported Features: no SCO / HV / CVSD / eSCO.
      // Bit 51 (byte6 bit3) = Secure Simple Pairing — required by Android.
      complete({0x00, 0xff, 0xc7, 0x08, 0xf8, 0x9b, 0x1d, 0x79, 0x83});
      break;
    case 0x1004: {  // Read Local Extended Features
      // Page 0 must mirror Read Local Supported Features. Returning zeros here
      // clears SSP and trips btm_sec_dev_reset("only controllers with SSP…").
      const uint8_t page = cmd.size() >= 4 ? cmd[3] : 0;
      if (page == 0) {
        complete({0x00, 0x00, 0x01, 0xff, 0xc7, 0x08, 0xf8, 0x9b, 0x1d, 0x79, 0x83});
      } else if (page == 1) {
        // Host features: SSP Host, LE Host, Secure Connections Host.
        complete({0x00, 0x01, 0x01, 0x0b, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
      } else {
        complete({0x00, page, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
      }
      break;
    }
    case 0x1005:  // Read Buffer Size: ACL only, SCO MTU/packets = 0.
      complete({0x00, 0xfd, 0x03, 0x00, 0x0a, 0x00, 0x00, 0x00});
      break;
    case 0x1009:  // Read BD_ADDR
      complete({0x00, 0x02, 0x00, 0x00, 0xbb, 0x77, 0x01});
      break;
    case 0x0401:  // Inquiry: no devices.
      status(0x00);
      mCb->hciEventReceived({0x01, 0x01, 0x00});
      break;
    case 0x0402:  // Inquiry Cancel
      complete({0x00});
      break;
    case 0x2002:  // LE Read Buffer Size (v1)
      complete({0x00, 0xfb, 0x00, 0x0a});
      break;
    case 0x2003:  // LE Read Local Supported Features: encryption only, no ISO.
      complete({0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00});
      break;
    case 0x2007:  // LE Read Advertising Physical Channel Tx Power
      complete({0x00, 0x00});
      break;
    case 0x200f:  // LE Read Filter Accept List Size (White List)
      complete({0x00, 0x08});
      break;
    case 0x2018:  // LE Rand
      complete({0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77, 0x88});
      break;
    case 0x201c:  // LE Read Supported States: Status + 8-byte bitfield
      complete({0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0x03, 0x00, 0x00});
      break;
    case 0x202a:  // LE Read Resolving List Size
      complete({0x00, 0x08});
      break;
    case 0x202b:  // LE Read Suggested Default Data Length
      complete({0x00, 0x1b, 0x00, 0x48, 0x08});
      break;
    case 0x202f:  // LE Read Maximum Data Length
      complete({0x00, 0xfb, 0x00, 0x48, 0x08, 0xfb, 0x00, 0x48, 0x08});
      break;
    case 0x203a:  // LE Read Maximum Advertising Data Length
      complete({0x00, 0x1f, 0x00});
      break;
    case 0x203b:  // LE Read Number of Supported Advertising Sets
      complete({0x00, 0x01});
      break;
    case 0x204a:  // LE Read Periodic Advertiser List Size
      complete({0x00, 0x01});
      break;
    case 0x204b:  // LE Read Transmit Power
      complete({0x00, 0x00, 0x00});
      break;
    case 0x2059:  // LE Read Advertising Physical Channel Tx Power
      complete({0x00, 0x00});
      break;
    case 0x2060:  // LE Read Buffer Size V2 (ACL + ISO sizes; ISO=0)
      complete({0x00, 0xfb, 0x00, 0x0a, 0x00, 0x00, 0x00});
      break;
    default:
      // Host-controller / LE config writes (Set Event Mask, Write Scan Enable,
      // Set Adv Enable, …) complete with Status only. LE *read* commands that
      // need multi-byte return params are handled above — a bare {0x00} makes
      // libbluetooth ASSERT(complete_view.IsValid()) and crash-loop.
      if ((op & 0xfc00) == 0x0c00 || (op & 0xfc00) == 0x2000) {
        ALOGW("soft HCI: unhandled opcode 0x%04x; status-only Complete", op);
        complete({0x00});
      } else {
        complete({0x01});  // Unknown HCI Command
      }
      break;
  }
}

}  // namespace aidl::android::hardware::bluetooth::impl
