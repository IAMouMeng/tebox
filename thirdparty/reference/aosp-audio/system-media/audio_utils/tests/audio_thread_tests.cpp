/*
 * Copyright (C) 2023 The Android Open Source Project
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

#include <audio_utils/threads.h>
#include <gtest/gtest.h>
#include <thread>

using namespace android;
using namespace android::audio_utils;

TEST(audio_thread_tests, conversion) {
    EXPECT_EQ(120, kDefaultPrio);

    EXPECT_EQ(kMaxRtPrio, nice_to_unified_priority(kMinNice));
    EXPECT_EQ(kMaxPrio - 1, nice_to_unified_priority(kMaxNice));

    EXPECT_EQ(kMinNice, unified_priority_to_nice(kMaxRtPrio));
    EXPECT_EQ(kMaxNice, unified_priority_to_nice(kMaxPrio - 1));

    EXPECT_EQ(kMaxRtPrio - 1, unified_priority_to_rtprio(0));
    EXPECT_EQ(kMinRtPrio, unified_priority_to_rtprio(98));

    EXPECT_EQ(0, rtprio_to_unified_priority(kMaxRtPrio - 1));
    EXPECT_EQ(98, rtprio_to_unified_priority(kMinRtPrio));

    EXPECT_FALSE(is_cfs_priority(kMaxRtPrio-1));
    EXPECT_TRUE(is_cfs_priority(kMaxRtPrio));  // note the bound is exclusive

    EXPECT_TRUE(is_realtime_priority(kMaxRtPrio-1));
    EXPECT_FALSE(is_realtime_priority(kMaxRtPrio));  // the bound is exclusive.
}

TEST(audio_thread_tests, priority) {
    const auto tid = gettid_wrapper();
    const int priority = get_thread_priority(tid);
    ASSERT_GE(priority, 0);

    constexpr int kPriority110 = 110;
    EXPECT_EQ(NO_ERROR, set_thread_priority(tid, kPriority110));
    EXPECT_EQ(kPriority110, get_thread_priority(tid));

    constexpr int kPriority130 = 130;
    EXPECT_EQ(NO_ERROR, set_thread_priority(tid, kPriority130));
    EXPECT_EQ(kPriority130, get_thread_priority(tid));

    // Requires privilege to go RT.
    // constexpr int kPriority98 = 98;
    // EXPECT_EQ(NO_ERROR, set_thread_priority(tid, kPriority98));
    // EXPECT_EQ(kPriority98, get_thread_priority(tid));

    EXPECT_EQ(NO_ERROR, set_thread_priority(tid, priority));
}

TEST(audio_thread_tests, cpu_count) {
    const unsigned cpu_count = std::thread::hardware_concurrency();
    ASSERT_EQ(cpu_count, get_number_cpus());
}

TEST(audio_thread_tests, affinity) {
    constexpr pid_t self = 0;
    const int limit = std::min(get_number_cpus(), sizeof(uint64_t) * CHAR_BIT);
    for (int i = 0; i < limit; ++i) {
        uint64_t mask = 1ULL << i;
        const status_t result = set_thread_affinity(self, mask);
        ASSERT_EQ(NO_ERROR, result);
        EXPECT_EQ(mask, get_thread_affinity(self).to_ullong());
    }
}

TEST(audio_thread_tests, invalid_affinity) {
    constexpr pid_t self = 0;
    const int cpu_count = get_number_cpus();
    ASSERT_NE(NO_ERROR, set_thread_affinity(self, std::bitset<kMaxCpus>{}.set(cpu_count)));
}
