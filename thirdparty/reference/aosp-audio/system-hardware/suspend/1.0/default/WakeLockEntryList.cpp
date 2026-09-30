/*
 * Copyright (C) 2019 The Android Open Source Project
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include "WakeLockEntryList.h"

#include <android-base/file.h>
#include <android-base/logging.h>
#include <android-base/parseint.h>
#include <android-base/stringprintf.h>
#include <android/system/suspend/internal/ISuspendControlServiceInternal.h>
#include <suspend_service_flags.h>

#include <iomanip>

using android::base::ParseInt;
using android::base::ReadFdToString;
using android::base::Readlink;
using android::base::StringPrintf;
using suspend_service::flags::fast_kernel_wakelock_reporting;

using ISCSI = ::android::system::suspend::internal::ISuspendControlServiceInternal;

namespace android {
namespace system {
namespace suspend {
namespace V1_0 {

namespace {

struct BitAndFilename {
    int32_t bit;
    std::string filename;
};

const BitAndFilename FIELDS[] = {
    {-1, "name"},
    {ISCSI::WAKE_LOCK_INFO_ACTIVE_COUNT, "active_count"},
    {ISCSI::WAKE_LOCK_INFO_LAST_CHANGE, "last_change_ms"},
    {ISCSI::WAKE_LOCK_INFO_MAX_TIME, "max_time_ms"},
    {ISCSI::WAKE_LOCK_INFO_TOTAL_TIME, "total_time_ms"},
    {ISCSI::WAKE_LOCK_INFO_ACTIVE_TIME, "active_time_ms"},
    {ISCSI::WAKE_LOCK_INFO_EVENT_COUNT, "event_count"},
    {ISCSI::WAKE_LOCK_INFO_EXPIRE_COUNT, "expire_count"},
    {ISCSI::WAKE_LOCK_INFO_PREVENT_SUSPEND_TIME, "prevent_suspend_time_ms"},
    {ISCSI::WAKE_LOCK_INFO_WAKEUP_COUNT, "wakeup_count"},
};

}  // namespace

static std::ostream& operator<<(std::ostream& out, const WakeLockInfo& entry) {
    const char* sep = " | ";
    const char* notApplicable = "---";
    bool kernelWakelock = entry.isKernelWakelock;

    // clang-format off
    out << sep
        << std::left << std::setw(30) << entry.name << sep
        << std::right << std::setw(6)
        << ((kernelWakelock) ? notApplicable : std::to_string(entry.pid)) << sep
        << std::left << std::setw(6) << ((kernelWakelock) ? "Kernel" : "Native") << sep
        << std::left << std::setw(8) << ((entry.isActive) ? "Active" : "Inactive") << sep
        << std::right << std::setw(12) << entry.activeCount << sep
        << std::right << std::setw(12) << std::to_string(entry.totalTime) + "ms" << sep
        << std::right << std::setw(12) << std::to_string(entry.maxTime) + "ms" << sep
        << std::right << std::setw(12)
        << ((kernelWakelock) ? std::to_string(entry.eventCount) : notApplicable) << sep
        << std::right << std::setw(12)
        << ((kernelWakelock) ? std::to_string(entry.wakeupCount) : notApplicable) << sep
        << std::right << std::setw(12)
        << ((kernelWakelock) ? std::to_string(entry.expireCount) : notApplicable) << sep
        << std::right << std::setw(20)
        << ((kernelWakelock) ? std::to_string(entry.preventSuspendTime) + "ms" : notApplicable)
        << sep
        << std::right << std::setw(16) << std::to_string(entry.lastChange) + "ms" << sep;
    // clang-format on

    return out;
}

std::ostream& operator<<(std::ostream& out, const WakeLockEntryList& list) {
    std::vector<WakeLockInfo> wlStats;
    list.getWakeLockStats(ISCSI::WAKE_LOCK_INFO_ALL_FIELDS, &wlStats);
    int width = 194;
    const char* sep = " | ";
    std::stringstream ss;
    ss << "  " << std::setfill('-') << std::setw(width) << "\n";
    std::string div = ss.str();

    out << div;

    std::stringstream header;
    header << sep << std::right << std::setw(((width - 14) / 2) + 14) << "WAKELOCK STATS"
           << std::right << std::setw((width - 14) / 2) << sep << "\n";
    out << header.str();

    out << div;

    // Col names
    // clang-format off
    out << sep
        << std::left << std::setw(30) << "NAME" << sep
        << std::left << std::setw(6) << "PID" << sep
        << std::left << std::setw(6) << "TYPE" << sep
        << std::left << std::setw(8) << "STATUS" << sep
        << std::left << std::setw(12) << "ACTIVE COUNT" << sep
        << std::left << std::setw(12) << "TOTAL TIME" << sep
        << std::left << std::setw(12) << "MAX TIME" << sep
        << std::left << std::setw(12) << "EVENT COUNT" << sep
        << std::left << std::setw(12) << "WAKEUP COUNT" << sep
        << std::left << std::setw(12) << "EXPIRE COUNT" << sep
        << std::left << std::setw(20) << "PREVENT SUSPEND TIME" << sep
        << std::left << std::setw(16) << "LAST CHANGE" << sep
        << "\n";
    // clang-format on

    out << div;

    // Rows
    for (const WakeLockInfo& entry : wlStats) {
        out << entry << "\n";
    }

    out << div;
    return out;
}

/**
 * Returns the monotonic time in milliseconds.
 */
TimestampType getTimeNow() {
    timespec monotime;
    clock_gettime(CLOCK_MONOTONIC, &monotime);
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::nanoseconds{monotime.tv_nsec})
               .count() +
           std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::seconds{monotime.tv_sec})
               .count();
}

WakeLockEntryList::WakeLockEntryList(size_t capacity, unique_fd kernelWakelockStatsFd)
    : mCapacity(capacity), mKernelWakelockStatsFd(std::move(kernelWakelockStatsFd)) {}

/**
 * Evicts LRU from back of list if stats is at capacity.
 */
void WakeLockEntryList::evictIfFull() {
    static std::chrono::steady_clock::time_point lastWarningTime{};
    static std::chrono::steady_clock::time_point lastEvictionTime{};
    static long evictionCountSinceLastLog = 0;

    if (mStats.size() == mCapacity) {
        auto evictIt = mStats.end();
        std::advance(evictIt, -1);
        auto evictKey = std::make_pair(evictIt->name, evictIt->pid);
        mLookupTable.erase(evictKey);
        mStats.erase(evictIt);

        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        long long secondsSinceLastLog =
            std::chrono::duration_cast<std::chrono::seconds>(now - lastWarningTime).count();
        evictionCountSinceLastLog++;

        if (secondsSinceLastLog >= 5) {
            long long secondsSinceLastEvict =
                std::chrono::duration_cast<std::chrono::seconds>(now - lastEvictionTime).count();
            LOG(WARNING) << "WakeLock Stats: Stats capacity met " << evictionCountSinceLastLog
                         << " time(s) since last warning (" << secondsSinceLastLog
                         << " seconds ago). An eviction is occurring now, with the previous"
                         << " eviction occurring " << secondsSinceLastEvict
                         << " seconds ago. Consider adjusting capacity to avoid stats eviction.";
            lastWarningTime = now;
            evictionCountSinceLastLog = 0; // Reset the count
        }
        lastEvictionTime = now;
    }
}

/**
 * Inserts entry as MRU.
 */
void WakeLockEntryList::insertEntry(WakeLockInfo entry) {
    auto key = std::make_pair(entry.name, entry.pid);
    mStats.emplace_front(std::move(entry));
    mLookupTable[key] = mStats.begin();
}

/**
 * Removes entry from the stats list.
 */
void WakeLockEntryList::deleteEntry(std::list<WakeLockInfo>::iterator entry) {
    auto key = std::make_pair(entry->name, entry->pid);
    mLookupTable.erase(key);
    mStats.erase(entry);
}

/**
 * Creates and returns a native wakelock entry.
 */
WakeLockInfo WakeLockEntryList::createNativeEntry(const std::string& name, int pid,
                                                  TimestampType timeNow) const {
    WakeLockInfo info;

    info.name = name;
    // It only makes sense to create a new entry on initial activation of the lock.
    info.activeCount = 1;
    info.lastChange = timeNow;
    info.maxTime = 0;
    info.totalTime = 0;
    info.isActive = true;
    info.activeTime = 0;
    info.isKernelWakelock = false;

    info.pid = pid;

    info.eventCount = 0;
    info.expireCount = 0;
    info.preventSuspendTime = 0;
    info.wakeupCount = 0;

    return info;
}

/*
 * Checks whether a given directory entry is a stat file we're interested in.
 */
static bool isStatFile(const struct dirent* de) {
    const char* statName = de->d_name;
    if (!strcmp(statName, ".") || !strcmp(statName, "..") || !strcmp(statName, "device") ||
        !strcmp(statName, "power") || !strcmp(statName, "subsystem") ||
        !strcmp(statName, "uevent")) {
        return false;
    }
    return true;
}

/*
 * Creates and returns a kernel wakelock entry with data read from mKernelWakelockStatsFd
 */
WakeLockInfo WakeLockEntryList::createKernelEntry(const std::string& kwlId) const {
    WakeLockInfo info;

    info.activeCount = 0;
    info.lastChange = 0;
    info.maxTime = 0;
    info.totalTime = 0;
    info.isActive = false;
    info.activeTime = 0;
    info.isKernelWakelock = true;

    info.pid = -1;  // N/A

    info.eventCount = 0;
    info.expireCount = 0;
    info.preventSuspendTime = 0;
    info.wakeupCount = 0;

    unique_fd wakelockFd{TEMP_FAILURE_RETRY(
        openat(mKernelWakelockStatsFd, kwlId.c_str(), O_DIRECTORY | O_CLOEXEC | O_RDONLY))};
    if (wakelockFd < 0) {
        char buf[PATH_MAX];
        ssize_t data_length =
            readlinkat(mKernelWakelockStatsFd, kwlId.c_str(), buf, sizeof(buf) - 1);
        if (data_length <= 0 || strncmp(kwlId.c_str(), buf, kwlId.length()) == 0) {
            buf[0] = '\0';
        }
        PLOG(ERROR) << "Error opening kernel wakelock stats for: " << kwlId << " (" << buf << ")";
    }

    std::unique_ptr<DIR, decltype(&closedir)> wakelockDp(fdopendir(dup(wakelockFd.get())),
                                                         &closedir);
    if (wakelockDp) {
        struct dirent* de;
        while ((de = readdir(wakelockDp.get()))) {
            if (!isStatFile(de)) {
                continue;
            }

            std::string statName(de->d_name);
            unique_fd statFd{
                TEMP_FAILURE_RETRY(openat(wakelockFd, statName.c_str(), O_CLOEXEC | O_RDONLY))};
            if (statFd < 0) {
                PLOG(ERROR) << "Error opening " << statName << " for " << kwlId;
                continue;
            }

            std::string valStr;
            if (!ReadFdToString(statFd.get(), &valStr)) {
                PLOG(ERROR) << "Error reading " << statName << " for " << kwlId;
                continue;
            }

            // Trim newline
            valStr.erase(std::remove(valStr.begin(), valStr.end(), '\n'), valStr.end());

            if (statName == "name") {
                info.name = valStr;
                continue;
            }

            int64_t statVal;
            if (!ParseInt(valStr, &statVal)) {
                std::string path;
                if (Readlink(StringPrintf("/proc/self/fd/%d", statFd.get()), &path)) {
                    LOG(ERROR) << "Unexpected format for wakelock stat value (" << valStr
                               << ") from file: " << path;
                } else {
                    LOG(ERROR) << "Unexpected format for wakelock stat value (" << valStr << ")";
                }
                continue;
            }

            if (statName == "active_count") {
                info.activeCount = statVal;
            } else if (statName == "active_time_ms") {
                info.activeTime = statVal;
            } else if (statName == "event_count") {
                info.eventCount = statVal;
            } else if (statName == "expire_count") {
                info.expireCount = statVal;
            } else if (statName == "last_change_ms") {
                info.lastChange = statVal;
            } else if (statName == "max_time_ms") {
                info.maxTime = statVal;
            } else if (statName == "prevent_suspend_time_ms") {
                info.preventSuspendTime = statVal;
            } else if (statName == "total_time_ms") {
                info.totalTime = statVal;
            } else if (statName == "wakeup_count") {
                info.wakeupCount = statVal;
            }
        }
    }

    // Derived stats
    info.isActive = info.activeTime > 0;

    return info;
}

/*
 * Creates and returns a kernel wakelock entry with data read from mKernelWakelockStatsFd.
 * Has been micro-optimized to reduce CPU time and wall time.
 */
WakeLockInfo WakeLockEntryList::createKernelEntry(ScratchSpace* ss, int wakeLockInfoFieldBitMask,
                                                  const std::string& kwlId) const {
    WakeLockInfo info;

    info.activeCount = 0;
    info.lastChange = 0;
    info.maxTime = 0;
    info.totalTime = 0;
    info.isActive = false;
    info.activeTime = 0;
    info.isKernelWakelock = true;

    info.pid = -1;  // N/A

    info.eventCount = 0;
    info.expireCount = 0;
    info.preventSuspendTime = 0;
    info.wakeupCount = 0;

    for (const auto& field : FIELDS) {
        const bool isNameField = field.bit == -1;
        if (!isNameField && (wakeLockInfoFieldBitMask & field.bit) == 0) {
            continue;
        }

        ss->statName = kwlId + "/" + field.filename;
        int statFd = -1;

        {
            std::lock_guard<std::mutex> lock(mLock);
            // Check if we have a valid cached file descriptor.
            auto it = mFdCache.find(ss->statName);
            if (it != mFdCache.end() && it->second >= 0) {
                auto result = lseek(it->second, 0, SEEK_SET);
                if (result < 0) {
                    PLOG(ERROR) << "Could not seek to start of FD for " << ss->statName;
                    mFdCache.erase(it);
                    PLOG(ERROR) << "Closed the FD.";
                } else {
                    statFd = it->second;
                }
            }

            if (statFd == -1) {
                unique_fd tmpFd(TEMP_FAILURE_RETRY(
                    openat(mKernelWakelockStatsFd, ss->statName.c_str(), O_CLOEXEC | O_RDONLY)));
                if (tmpFd < 0) {
                    PLOG(ERROR) << "Error opening " << ss->statName << " for " << kwlId;
                    continue;
                }
                statFd = tmpFd;
                mFdCache.insert(it, {ss->statName, std::move(tmpFd)});
            }
        }  // mLock is released here

        ss->valStr.clear();
        ssize_t n;
        while ((n = TEMP_FAILURE_RETRY(read(statFd, &ss->readBuff[0], sizeof(ss->readBuff)))) > 0) {
            ss->valStr.append(ss->readBuff, n);
        }
        if (n < 0) {
            PLOG(ERROR) << "Error reading " << ss->statName;
            {
                std::lock_guard<std::mutex> lock(mLock);
                mFdCache.erase(ss->statName);
                PLOG(ERROR) << "Closed the FD.";
            }
            continue;
        }

        // Trim newline
        ss->valStr.erase(std::remove(ss->valStr.begin(), ss->valStr.end(), '\n'), ss->valStr.end());

        if (isNameField) {
            info.name = ss->valStr;
            continue;
        }

        int64_t statVal;
        if (!ParseInt(ss->valStr, &statVal)) {
            std::string path;
            if (Readlink(StringPrintf("/proc/self/fd/%d", statFd), &path)) {
                LOG(ERROR) << "Unexpected format for wakelock stat value (" << ss->valStr
                           << ") from file: " << path;
            } else {
                LOG(ERROR) << "Unexpected format for wakelock stat value (" << ss->valStr << ")";
            }
            continue;
        }

        if (field.filename == "active_count") {
            info.activeCount = statVal;
        } else if (field.filename == "active_time_ms") {
            info.activeTime = statVal;
        } else if (field.filename == "event_count") {
            info.eventCount = statVal;
        } else if (field.filename == "expire_count") {
            info.expireCount = statVal;
        } else if (field.filename == "last_change_ms") {
            info.lastChange = statVal;
        } else if (field.filename == "max_time_ms") {
            info.maxTime = statVal;
        } else if (field.filename == "prevent_suspend_time_ms") {
            info.preventSuspendTime = statVal;
        } else if (field.filename == "total_time_ms") {
            info.totalTime = statVal;
        } else if (field.filename == "wakeup_count") {
            info.wakeupCount = statVal;
        }
    }

    // Derived stats
    info.isActive = info.activeTime > 0;

    return info;
}

void WakeLockEntryList::getKernelWakelockStats(int wakeLockInfoFieldBitMask,
                                               std::vector<WakeLockInfo>* aidl_return) const {
    std::unique_ptr<DIR, decltype(&closedir)> dp(fdopendir(dup(mKernelWakelockStatsFd.get())),
                                                 &closedir);
    if (dp) {
        // rewinddir, else subsequent calls will not get any kernel wakelocks.
        rewinddir(dp.get());

        ScratchSpace ss;
        struct dirent* de;
        while ((de = readdir(dp.get()))) {
            std::string kwlId(de->d_name);
            if ((kwlId == ".") || (kwlId == "..")) {
                continue;
            }
            WakeLockInfo entry = fast_kernel_wakelock_reporting()
                                     ? createKernelEntry(&ss, wakeLockInfoFieldBitMask, kwlId)
                                     : createKernelEntry(kwlId);

            aidl_return->emplace_back(std::move(entry));
        }
    }
}

void WakeLockEntryList::updateOnAcquire(const std::string& name, int pid) {
    TimestampType timeNow = getTimeNow();

    std::lock_guard<std::mutex> lock(mLock);

    auto key = std::make_pair(name, pid);
    auto it = mLookupTable.find(key);
    if (it == mLookupTable.end()) {
        evictIfFull();
        WakeLockInfo newEntry = createNativeEntry(name, pid, timeNow);
        insertEntry(newEntry);
    } else {
        auto staleEntry = it->second;
        WakeLockInfo updatedEntry = *staleEntry;

        // Update entry
        updatedEntry.isActive = true;
        updatedEntry.activeTime = 0;
        updatedEntry.activeCount++;
        updatedEntry.lastChange = timeNow;

        deleteEntry(staleEntry);
        insertEntry(std::move(updatedEntry));
    }
}

void WakeLockEntryList::updateOnRelease(const std::string& name, int pid) {
    TimestampType timeNow = getTimeNow();

    std::lock_guard<std::mutex> lock(mLock);

    auto key = std::make_pair(name, pid);
    auto it = mLookupTable.find(key);
    if (it == mLookupTable.end()) {
        LOG(INFO) << "WakeLock Stats: A stats entry for, \"" << name
                  << "\" was not found. This is most likely due to it being evicted.";
    } else {
        auto staleEntry = it->second;
        WakeLockInfo updatedEntry = *staleEntry;

        // Update entry
        TimestampType timeDelta = timeNow - updatedEntry.lastChange;
        if (updatedEntry.activeCount > 0) {
            updatedEntry.activeCount--;
        } else {
            LOG(ERROR) << "WakeLock Stats: Active count attempted to go below zero for "
                       << "wakelock \"" << name << "\". This is unexpected.";
        }
        updatedEntry.isActive = (updatedEntry.activeCount > 0);
        updatedEntry.activeTime += timeDelta;
        updatedEntry.maxTime = std::max(updatedEntry.maxTime, updatedEntry.activeTime);
        updatedEntry.activeTime = updatedEntry.isActive ? updatedEntry.activeTime : 0;
        updatedEntry.totalTime += timeDelta;
        updatedEntry.lastChange = timeNow;

        deleteEntry(staleEntry);
        insertEntry(std::move(updatedEntry));
    }
}
/**
 * Updates the native wakelock stats based on the current time.
 */
void WakeLockEntryList::updateNow() {
    std::lock_guard<std::mutex> lock(mLock);

    TimestampType timeNow = getTimeNow();

    for (std::list<WakeLockInfo>::iterator it = mStats.begin(); it != mStats.end(); ++it) {
        if (it->isActive) {
            TimestampType timeDelta = timeNow - it->lastChange;
            it->activeTime += timeDelta;
            it->maxTime = std::max(it->maxTime, it->activeTime);
            it->totalTime += timeDelta;
            it->lastChange = timeNow;
        }
    }
}

void WakeLockEntryList::getWakeLockStats(int wakeLockInfoFieldBitMask,
                                         std::vector<WakeLockInfo>* aidl_return) const {
    // Under no circumstances should the lock be held while getting kernel wakelock stats
    {
        std::lock_guard<std::mutex> lock(mLock);
        for (const WakeLockInfo& entry : mStats) {
            aidl_return->emplace_back(entry);
        }
    }
    getKernelWakelockStats(wakeLockInfoFieldBitMask, aidl_return);
}

}  // namespace V1_0
}  // namespace suspend
}  // namespace system
}  // namespace android
