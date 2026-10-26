#pragma once

#include <aidl/android/hardware/media/c2/BaseBlock.h>
#include <aidl/android/hardware/media/c2/FrameData.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace gki {

/* Pack |data| into a Codec2 linear nativeBlock (+ RangeInfo) on |output|. */
bool makeOutputBlock(const std::vector<uint8_t>& data,
                     std::vector<aidl::android::hardware::media::c2::BaseBlock>* baseBlocks,
                     aidl::android::hardware::media::c2::FrameData* output);

/* Read bytes from a linear BaseBlock (C2Handle size ints or whole fd). */
std::vector<uint8_t> readLinearBlock(
        const aidl::android::hardware::media::c2::BaseBlock& block,
        size_t maxBytes = 4u << 20);

/* Concatenate linear input buffers (dma-buf / ashmem) into |out|. */
bool readInputFrame(const aidl::android::hardware::media::c2::FrameData& frame,
                    const std::vector<aidl::android::hardware::media::c2::BaseBlock>& baseBlocks,
                    std::vector<uint8_t>* out);

}  // namespace gki

