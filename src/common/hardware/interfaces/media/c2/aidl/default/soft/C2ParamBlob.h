#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

namespace gki {

// C2Hal_RangeInfo (codec2/common/BufferTypes.h): the bytes of a linear block.
constexpr uint32_t kRangeInfoIndex = 0xE0000000u;
// C2Hal_RectInfo: the crop of a graphic block.
constexpr uint32_t kRectInfoIndex = 0xE0000001u;
// C2StreamPictureSizeInfo::output, C2StreamCropRectInfo::output.
constexpr uint32_t kPictureSizeOut = 0xD2001800u;
constexpr uint32_t kCropRectOut = 0xD2001801u;

/* Append a C2Param (u32 size, u32 index, payload) to |blob|, 8-byte aligned. */
void appendParam(std::vector<uint8_t>* blob, uint32_t index, const void* payload, size_t size);

bool findParam(const std::vector<uint8_t>& blob, uint32_t index, void* out, size_t size);

}  // namespace gki
