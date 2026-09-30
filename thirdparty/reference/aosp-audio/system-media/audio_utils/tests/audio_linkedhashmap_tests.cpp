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

#include <audio_utils/linked_hash_map.h>

#include <gtest/gtest.h>

using android::audio_utils::linked_hash_map;

TEST(linked_hash_map, basic) {
    linked_hash_map<int, int> lhm;

    // assignment
    lhm[10] = 1;
    lhm[20] = 2;
    lhm[30] = 3;

    // access by key
    ASSERT_EQ(1, lhm[10]);
    ASSERT_EQ(2, lhm[20]);
    ASSERT_EQ(3, lhm[30]);

    // iterates in insertion order
    auto it = lhm.begin();
    ASSERT_EQ(1UL, it->second);
    ++it;
    ASSERT_EQ(2UL, it->second);
    ++it;
    ASSERT_EQ(3UL, it->second);
    ++it;
    ASSERT_EQ(lhm.end(), it);

    // correct size
    ASSERT_EQ(3UL, lhm.size());

    // invalid key search returns end().
    it = lhm.find(22);
    ASSERT_EQ(lhm.end(), it);

    // valid key search returns proper iterator.
    it = lhm.find(20);
    ASSERT_EQ(20, it->first);
    ASSERT_EQ(2, it->second);

    // test deletion
    lhm.erase(it);
    it = lhm.find(20);
    ASSERT_EQ(lhm.end(), it);

    // correct size
    ASSERT_EQ(2UL, lhm.size());

    // iterates in order
    it = lhm.begin();
    ASSERT_EQ(1UL, it->second);
    ++it;
    ASSERT_EQ(3UL, it->second);
    ++it;
    ASSERT_EQ(lhm.end(), it);

    // add new value
    lhm[2] = -1;

    ASSERT_EQ(-1, lhm[2]);

    // iterates in order of insertion
    it = lhm.begin();
    ASSERT_EQ(1UL, it->second);
    ++it;
    ASSERT_EQ(3UL, it->second);
    ++it;
    ASSERT_EQ(-1UL, it->second);
    ++it;
    ASSERT_EQ(lhm.end(), it);
}

TEST(linked_hash_map, structural) {
    linked_hash_map<int, int> lhm;

    // assignment
    lhm[10] = 1;
    lhm[20] = 2;
    lhm[30] = 3;

    // exercise default copy ctor (or move ctor)
    auto lhm2 = lhm;

    // exercise comparator
    ASSERT_EQ(lhm, lhm2);

    // access by key
    ASSERT_EQ(1, lhm2[10]);
    ASSERT_EQ(2, lhm2[20]);
    ASSERT_EQ(3, lhm2[30]);
}
