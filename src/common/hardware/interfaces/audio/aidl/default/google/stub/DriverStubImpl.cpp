/*
 * Copyright (C) 2024 The Android Open Source Project
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

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <mutex>
#include <string>
#include <unistd.h>
#include <vector>

#define LOG_TAG "AHAL_Stream"
#include <Utils.h>
#include <android-base/logging.h>
#include <audio_utils/clock.h>

#include "core-impl/DriverStubImpl.h"

using aidl::android::hardware::audio::common::getChannelCount;
using aidl::android::hardware::audio::common::getPcmSampleSizeInBytes;
using aidl::android::media::audio::common::AudioFormatType;

namespace {
class VirtioPort {
    const char* name_;
    std::mutex mutex_;
    int fd_ = -1;
    bool openLocked() {
        if (fd_ >= 0) return true;
        DIR* dir = opendir("/sys/class/virtio-ports");
        if (!dir) return false;
        while (dirent* e = readdir(dir)) {
            if (e->d_name[0] == '.') continue;
            std::ifstream f(std::string("/sys/class/virtio-ports/") + e->d_name + "/name");
            std::string n;
            if (std::getline(f, n) && n == name_) {
                fd_ = ::open((std::string("/dev/") + e->d_name).c_str(),
                             O_RDWR | O_NONBLOCK | O_CLOEXEC);
                break;
            }
        }
        closedir(dir);
        return fd_ >= 0;
    }

  public:
    explicit VirtioPort(const char* name) : name_(name) {}
    void write(const void* data, size_t bytes) {
        std::lock_guard lock(mutex_);
        if (!openLocked()) return;
        const auto* p = static_cast<const uint8_t*>(data);
        while (bytes > 0) {
            ssize_t n = ::write(fd_, p, bytes);
            if (n > 0) {
                p += n;
                bytes -= static_cast<size_t>(n);
                continue;
            }
            if (n < 0 && errno == EINTR) continue;
            break;
        }
    }
    void read(void* data, size_t bytes) {
        std::lock_guard lock(mutex_);
        size_t got = 0;
        if (openLocked()) {
            while (got < bytes) {
                ssize_t n = ::read(fd_, static_cast<uint8_t*>(data) + got, bytes - got);
                if (n > 0) {
                    got += static_cast<size_t>(n);
                    continue;
                }
                if (n < 0 && errno == EINTR) continue;
                break;
            }
        }
        got -= got % 2;
        memset(static_cast<uint8_t*>(data) + got, 0, bytes - got);
    }
};
VirtioPort gSpeaker("tebox.audio_out");
VirtioPort gMic("tebox.audio_in");

// Remote submix loopback for AudioPlaybackCapture / scrcpy --audio-source=output.
// OUT_SUBMIX writes here; IN_SUBMIX reads it. Speaker output is also tapped when
// no r_submix output is open, so capture still works if AF only opens the input.
//
// AOSP MonoPipe: writer blocks when the pipe is full (if a reader is open);
// reader blocks until data arrives. A 1.0x-buffer read timeout races QEMU
// scheduling and underruns every few hundred ms (滋啦). Dropping on overflow
// also clicks. Wait instead.
class SubmixPipe {
    std::mutex mutex_;
    std::condition_variable cv_;
    // ~85 ms at 48 kHz stereo s16 (AOSP kDefaultPipeSizeInFrames). A 1 s
    // ring plus IN pacing kept a full second of backlog, so scrcpy lagged.
    std::vector<uint8_t> ring_ = std::vector<uint8_t>(4096 * 4);
    size_t head_ = 0, size_ = 0;
    std::atomic<int> outputs_{0};
    std::atomic<int> inputs_{0};
    int16_t lastL_ = 0, lastR_ = 0;

    size_t spaceLocked() const { return ring_.size() - size_; }

    void dropOldestLocked(size_t bytes) {
        bytes = std::min(bytes, size_ - size_ % 4);
        head_ = (head_ + bytes) % ring_.size();
        size_ -= bytes;
    }

    void appendLocked(const uint8_t* p, size_t bytes) {
        const size_t tail = (head_ + size_) % ring_.size();
        const size_t first = std::min(bytes, ring_.size() - tail);
        std::memcpy(&ring_[tail], p, first);
        if (bytes > first) std::memcpy(&ring_[0], p + first, bytes - first);
        size_ += bytes;
    }

  public:
    void addOutput() { outputs_.fetch_add(1, std::memory_order_relaxed); }
    void removeOutput() { outputs_.fetch_sub(1, std::memory_order_relaxed); }
    bool hasOutput() const { return outputs_.load(std::memory_order_relaxed) > 0; }
    int outputCount() const { return outputs_.load(std::memory_order_relaxed); }
    void addInput() { inputs_.fetch_add(1, std::memory_order_relaxed); }
    void removeInput() { inputs_.fetch_sub(1, std::memory_order_relaxed); }
    bool hasInput() const { return inputs_.load(std::memory_order_relaxed) > 0; }

    void flush() {
        std::lock_guard lock(mutex_);
        head_ = 0;
        size_ = 0;
        cv_.notify_all();
    }

    size_t available() {
        std::lock_guard lock(mutex_);
        return size_;
    }

    // Never block the writer (speaker tap shares this pipe). Drop oldest on
    // overflow so capture stays near live.
    void write(const void* data, size_t bytes, int /*timeoutUs*/ = 0) {
        std::lock_guard lock(mutex_);
        const auto* p = static_cast<const uint8_t*>(data);
        bytes -= bytes % 4;
        if (bytes == 0) return;
        if (bytes >= ring_.size()) {
            p += bytes - ring_.size();
            bytes = ring_.size();
            head_ = 0;
            size_ = 0;
            appendLocked(p, bytes);
            cv_.notify_all();
            return;
        }
        if (spaceLocked() < bytes) {
            dropOldestLocked((bytes - spaceLocked() + 3) & ~size_t{3});
        }
        appendLocked(p, bytes);
        cv_.notify_all();
    }

    // Wait up to timeoutUs for a full buffer. Remainder holds the last stereo
    // frame only after that (true silence / no writer).
    void read(void* data, size_t bytes, int timeoutUs) {
        std::unique_lock lock(mutex_);
        bytes -= bytes % 4;
        if (bytes == 0) return;
        if (timeoutUs > 0 && size_ < bytes) {
            cv_.wait_until(lock,
                           std::chrono::steady_clock::now() + std::chrono::microseconds(timeoutUs),
                           [&] { return size_ >= bytes; });
        }
        auto* p = static_cast<uint8_t*>(data);
        size_t got = std::min(bytes, size_ - size_ % 4);
        size_t remaining = got;
        uint8_t* dst = p;
        while (remaining > 0) {
            const size_t n = std::min(remaining, ring_.size() - head_);
            std::memcpy(dst, &ring_[head_], n);
            head_ = (head_ + n) % ring_.size();
            dst += n;
            remaining -= n;
        }
        size_ -= got;
        cv_.notify_all();
        if (got >= 4) {
            std::memcpy(&lastL_, p + got - 4, 2);
            std::memcpy(&lastR_, p + got - 2, 2);
        }
        for (size_t i = got; i + 3 < bytes; i += 4) {
            std::memcpy(p + i, &lastL_, 2);
            std::memcpy(p + i + 2, &lastR_, 2);
        }
    }
};
SubmixPipe gSubmix;

constexpr int kPipeRate = 48000;

void writeStereoToPipe(const int16_t* stereo, size_t frames, int rate) {
    if (frames == 0) return;
    if (rate <= 0) rate = kPipeRate;
    if (rate == kPipeRate) {
        gSubmix.write(stereo, frames * 2 * sizeof(int16_t));
        return;
    }
    const size_t outFrames = (frames * static_cast<size_t>(kPipeRate) + static_cast<size_t>(rate) / 2) /
                             static_cast<size_t>(rate);
    std::vector<int16_t> tmp(outFrames * 2);
    for (size_t i = 0; i < outFrames; ++i) {
        size_t src = (i * static_cast<size_t>(rate)) / static_cast<size_t>(kPipeRate);
        if (src >= frames) src = frames - 1;
        tmp[2 * i] = stereo[2 * src];
        tmp[2 * i + 1] = stereo[2 * src + 1];
    }
    gSubmix.write(tmp.data(), tmp.size() * sizeof(int16_t));
}

// Wait up to 2x the burst, clamped so a cold start cannot stall AF 200 ms
// (that warped PTS and turned the 440 Hz tone into 滋啦).
int submixReadTimeoutUs(size_t frames, int rate) {
    if (rate <= 0) rate = kPipeRate;
    const int twoBuf = static_cast<int>((frames * 2000000ULL) / static_cast<unsigned>(rate));
    if (twoBuf < 10000) return 10000;
    if (twoBuf > 40000) return 40000;
    return twoBuf;
}

void readStereoFromPipe(int16_t* stereo, size_t frames, int rate) {
    if (frames == 0) return;
    if (rate <= 0) rate = kPipeRate;
    const int timeoutUs = submixReadTimeoutUs(frames, rate);
    if (rate == kPipeRate) {
        gSubmix.read(stereo, frames * 2 * sizeof(int16_t), timeoutUs);
        return;
    }
    const size_t inFrames = (frames * static_cast<size_t>(kPipeRate) + static_cast<size_t>(rate) / 2) /
                            static_cast<size_t>(rate);
    std::vector<int16_t> tmp(inFrames * 2);
    gSubmix.read(tmp.data(), tmp.size() * sizeof(int16_t), timeoutUs);
    for (size_t i = 0; i < frames; ++i) {
        size_t src = inFrames == 0 ? 0 : (i * static_cast<size_t>(kPipeRate)) / static_cast<size_t>(rate);
        if (src >= inFrames) src = inFrames - 1;
        stereo[2 * i] = tmp[2 * src];
        stereo[2 * i + 1] = tmp[2 * src + 1];
    }
}
}  // namespace

namespace aidl::android::hardware::audio::core {

DriverStubImpl::DriverStubImpl(const StreamContext& context, int asyncSleepTimeUs, bool isSubmix)
    : mBufferSizeFrames(context.getBufferSizeInFrames()),
      mFrameSizeBytes(context.getFrameSize()),
      mChannelCount(getChannelCount(context.getChannelLayout())),
      mBytesPerSample(getPcmSampleSizeInBytes(context.getFormat().pcm)),
      mIsPcm(context.getFormat().type == AudioFormatType::PCM),
      mSampleRate(context.getSampleRate()),
      mIsAsynchronous(!!context.getAsyncCallback()),
      mIsInput(context.isInput()),
      mMixPortHandle(context.getMixPortHandle()),
      mAsyncSleepTimeUs(asyncSleepTimeUs),
      mIsSubmix(isSubmix) {}

#define LOG_ENTRY()                                                                          \
    LOG(DEBUG) << "[" << (mIsInput ? "in" : "out") << "|ioHandle:" << mMixPortHandle << "] " \
               << __func__;

::android::status_t DriverStubImpl::init(DriverCallbackInterface* /*callback*/) {
    LOG_ENTRY();
    mIsInitialized = true;
    return ::android::OK;
}

::android::status_t DriverStubImpl::drain(StreamDescriptor::DrainMode) {
    LOG_ENTRY();
    if (!mIsInitialized) {
        LOG(FATAL) << __func__ << ": must not happen for an uninitialized driver";
    }
    if (!mIsInput) {
        if (!mIsAsynchronous) {
            static constexpr float kMicrosPerSecond = MICROS_PER_SECOND;
            const size_t delayUs = static_cast<size_t>(
                    std::roundf(mBufferSizeFrames * kMicrosPerSecond / mSampleRate));
            usleep(delayUs);
        } else if (mAsyncSleepTimeUs) {
            usleep(mAsyncSleepTimeUs);
        }
    }
    return ::android::OK;
}

::android::status_t DriverStubImpl::flush() {
    LOG_ENTRY();
    if (!mIsInitialized) {
        LOG(FATAL) << __func__ << ": must not happen for an uninitialized driver";
    }
    return ::android::OK;
}

::android::status_t DriverStubImpl::pause() {
    LOG_ENTRY();
    if (!mIsInitialized) {
        LOG(FATAL) << __func__ << ": must not happen for an uninitialized driver";
    }
    return ::android::OK;
}

::android::status_t DriverStubImpl::standby() {
    LOG_ENTRY();
    if (!mIsInitialized) {
        LOG(FATAL) << __func__ << ": must not happen for an uninitialized driver";
    }
    mIsStandby = true;
    return ::android::OK;
}

::android::status_t DriverStubImpl::start() {
    LOG_ENTRY();
    if (!mIsInitialized) {
        LOG(FATAL) << __func__ << ": must not happen for an uninitialized driver";
    }
    mIsStandby = false;
    mStartTimeNs = ::android::uptimeNanos();
    mFramesSinceStart = 0;
    if (mIsSubmix && !mIsInput && !mSubmixOutputCounted) {
        gSubmix.flush();
        gSubmix.addOutput();
        mSubmixOutputCounted = true;
        LOG(INFO) << "r_submix output started, loopback writers=" << gSubmix.outputCount();
    }
    if (mIsSubmix && mIsInput && !mSubmixInputCounted) {
        gSubmix.flush();
        gSubmix.addInput();
        mSubmixInputCounted = true;
        LOG(INFO) << "r_submix input started";
    }
    return ::android::OK;
}

::android::status_t DriverStubImpl::transfer(void* buffer, size_t frameCount,
                                             size_t* actualFrameCount, int32_t* latencyMs) {
    // No LOG_ENTRY as this is called very often.
    if (!mIsInitialized) {
        LOG(FATAL) << __func__ << ": must not happen for an uninitialized driver";
    }
    if (mIsStandby) {
        LOG(FATAL) << __func__ << ": must not happen while in standby";
    }
    *actualFrameCount = frameCount;
    if (latencyMs) {
        int32_t ms = static_cast<int32_t>((mBufferSizeFrames * 1000 + mSampleRate - 1) / mSampleRate);
        *latencyMs = ms < 10 ? 10 : ms;
    }
    auto paceRealtime = [&]() {
        if (mSampleRate <= 0) return;
        mFramesSinceStart += *actualFrameCount;
        const long bufferDurationUs = (*actualFrameCount) * MICROS_PER_SECOND / mSampleRate;
        const auto totalDurationUs =
                (::android::uptimeNanos() - mStartTimeNs) / NANOS_PER_MICROSECOND;
        const long totalOffsetUs =
                mFramesSinceStart * MICROS_PER_SECOND / mSampleRate - totalDurationUs;
        if (totalOffsetUs > 0) {
            usleep(std::min(totalOffsetUs, bufferDurationUs));
        }
    };
    // r_submix: write/read first (homemade + AOSP MonoPipe), then real-time
    // pace. The 500 us async stub sleep makes the pipe overflow and crackle.
    if (mIsSubmix) {
        // I/O below, then paceRealtime() before return.
    } else if (mIsAsynchronous) {
        if (mAsyncSleepTimeUs) usleep(mAsyncSleepTimeUs);
    } else {
        paceRealtime();
    }
    // tebox-cast audio-out.sock is PCM s16le 48 kHz stereo; audio-in.sock is
    // s16le 48 kHz mono. r_submix loopback is s16le stereo (same as homemade).
    // Convert whatever mix format AudioFlinger chose.
    const size_t bytes = frameCount * mFrameSizeBytes;
    if (!mIsPcm || mBytesPerSample == 0) {
        if (mIsInput) {
            if (mIsSubmix) {
                gSubmix.read(buffer, bytes, submixReadTimeoutUs(frameCount, mSampleRate));
            } else {
                gMic.read(buffer, bytes);
            }
        } else if (mIsSubmix) {
            gSubmix.write(buffer, bytes);
        } else {
            gSpeaker.write(buffer, bytes);
            if (!gSubmix.hasOutput()) gSubmix.write(buffer, bytes);
        }
        if (mIsSubmix && !(mIsInput && gSubmix.available() > 3840)) paceRealtime();
        return ::android::OK;
    }
    const size_t channels = mChannelCount ? mChannelCount : 1;
    const size_t bps = mBytesPerSample;
    auto packSample = [&](uint8_t* p, int16_t s) {
        if (bps == 2) {
            std::memcpy(p, &s, 2);
        } else if (bps == 3) {
            const int32_t v = static_cast<int32_t>(s) << 8;
            p[0] = static_cast<uint8_t>(v);
            p[1] = static_cast<uint8_t>(v >> 8);
            p[2] = static_cast<uint8_t>(v >> 16);
        } else if (bps == 4) {
            const int32_t v = static_cast<int32_t>(s) << 16;
            std::memcpy(p, &v, 4);
        }
    };
    if (mIsInput) {
        auto* dst = static_cast<uint8_t*>(buffer);
        if (mIsSubmix) {
            mCastPcm.resize(frameCount * 2);
            readStereoFromPipe(mCastPcm.data(), frameCount, mSampleRate);
            for (size_t i = 0; i < frameCount; ++i) {
                const int16_t l = mCastPcm[2 * i];
                const int16_t r = mCastPcm[2 * i + 1];
                for (size_t ch = 0; ch < channels; ++ch) {
                    packSample(dst + (i * channels + ch) * bps, (ch == 1 && channels > 1) ? r : l);
                }
            }
        } else {
            mCastPcm.resize(frameCount);
            gMic.read(mCastPcm.data(), frameCount * sizeof(int16_t));
            for (size_t i = 0; i < frameCount; ++i) {
                const int16_t s = mCastPcm[i];
                for (size_t ch = 0; ch < channels; ++ch) {
                    packSample(dst + (i * channels + ch) * bps, s);
                }
            }
        }
    } else {
        mCastPcm.resize(frameCount * 2);
        const auto* src = static_cast<const uint8_t*>(buffer);
        auto sample = [&](size_t frame, size_t ch) -> int16_t {
            const uint8_t* p = src + (frame * channels + ch) * bps;
            if (bps == 2) {
                int16_t v;
                std::memcpy(&v, p, 2);
                return v;
            }
            if (bps == 3) {
                int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
                if (v & 0x800000) v |= static_cast<int32_t>(0xff000000);
                return static_cast<int16_t>(v >> 8);
            }
            if (bps == 4) {
                int32_t v;
                std::memcpy(&v, p, 4);
                return static_cast<int16_t>(v >> 16);
            }
            return 0;
        };
        for (size_t i = 0; i < frameCount; ++i) {
            const int16_t l = sample(i, 0);
            const int16_t r = channels > 1 ? sample(i, 1) : l;
            mCastPcm[2 * i] = l;
            mCastPcm[2 * i + 1] = r;
        }
        const size_t pcmBytes = mCastPcm.size() * sizeof(int16_t);
        if (mIsSubmix) {
            writeStereoToPipe(mCastPcm.data(), frameCount, mSampleRate);
        } else {
            gSpeaker.write(mCastPcm.data(), pcmBytes);
            if (!gSubmix.hasOutput()) {
                writeStereoToPipe(mCastPcm.data(), frameCount, mSampleRate);
            }
        }
    }
    // Both sides pace from frames-vs-wall so scrcpy PTS stays honest. If IN
    // still has >20 ms buffered, skip the sleep and catch up to live audio.
    if (mIsSubmix && !(mIsInput && gSubmix.available() > 3840)) paceRealtime();
    return ::android::OK;
}

void DriverStubImpl::shutdown() {
    LOG_ENTRY();
    if (mSubmixOutputCounted) {
        gSubmix.removeOutput();
        mSubmixOutputCounted = false;
    }
    if (mSubmixInputCounted) {
        gSubmix.removeInput();
        mSubmixInputCounted = false;
    }
    mIsInitialized = false;
}

}  // namespace aidl::android::hardware::audio::core
