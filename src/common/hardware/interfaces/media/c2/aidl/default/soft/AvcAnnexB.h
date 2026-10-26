#pragma once

#include <cstdint>
#include <vector>

namespace gki {

bool avcHasIdr(const std::vector<uint8_t>& annexB);
std::vector<uint8_t> extractAvcCsd(const std::vector<uint8_t>& annexB);

}  // namespace gki
