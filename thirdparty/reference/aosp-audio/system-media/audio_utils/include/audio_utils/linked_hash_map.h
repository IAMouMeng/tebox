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

#pragma once

#include <list>
#include <unordered_map>

namespace android::audio_utils {

/**
 * linked_hash_map
 *
 * A hash map that iterates in order of oldest to newest inserted.
 * See also Java LinkedHashMap.
 *
 * O(1) lookup, insertion, deletion, iteration (with std::unordered_map and std::list)
 *
 * This can be used to hold historical records indexed on a key,
 * whose container size can be controlled by evicting the least recently used record.
 *
 * The class is not thread safe: Locking must occur at the caller.
 *
 * This is a basic implementation, many STL map methods are not implemented.
 *
 * @tparam K Key type
 * @tparam V Value type
 * @tparam M Map type (std::unordered_map or std::map)
 * @tparam L List type should have fast and stable iterator
 *           insertion and erasure.
 */
template<typename K, typename V,
        template<typename, typename, typename...> class M = std::unordered_map,
        template<typename, typename...> class L = std::list>
class linked_hash_map {
    using List = L<std::pair<K, V>>;

    // if K is large, could use a map with a reference_wrapper<K>.
    // or a set with an iterator_wrapper<List::iterator>.
    using Map = M<K, typename List::iterator>;

public:
    // The iterator returned is the list iterator.
    using iterator = List::iterator;

    // Equivalent linked hash maps must contain the same elements
    // inserted in the same order.
    bool operator==(const linked_hash_map<K, V, M, L>& other) const {
        return list_ == other.list_;
    }

    // The iterators returned are List::iterator.
    auto find(const K& k) {
        auto map_it = map_.find(k);
        if (map_it == map_.end()) return list_.end();
        return map_it->second;
    }

    auto erase(const List::iterator& it) {
        if (it != list_.end()) {
            map_.erase(it->first);
            return list_.erase(it);
        }
        return it;
    }

    auto size() const { return list_.size(); }
    auto begin() const { return list_.begin(); }
    auto end() const { return list_.end(); }

    auto begin() { return list_.begin(); }
    auto end() { return list_.end(); }
    template <typename KU>
    auto& operator[](KU&& k) {
        auto map_it = map_.find(k);
        if (map_it != map_.end()) return map_it->second->second;
        auto it = list_.insert(list_.end(),
                std::make_pair(std::forward<KU>(k), V{})); // oldest to newest.
        map_[it->first] = it;
        return it->second;
    }

private:
    Map map_;
    List list_;  // oldest is first.
};

} // namespace android::audio_utils
