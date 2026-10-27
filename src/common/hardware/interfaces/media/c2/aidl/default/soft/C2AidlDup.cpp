#include "C2AidlDup.h"

#include <android/hardware_buffer_aidl.h>
#include <unistd.h>

using aidl::android::hardware::HardwareBuffer;
using aidl::android::hardware::common::NativeHandle;
using aidl::android::hardware::media::c2::BaseBlock;
using aidl::android::hardware::media::c2::Buffer;
using aidl::android::hardware::media::c2::FrameData;
using aidl::android::hardware::media::c2::Work;

namespace gki {

NativeHandle duplicateHandle(const NativeHandle& src) {
    NativeHandle out;
    out.ints = src.ints;
    for (const auto& fd : src.fds) {
        out.fds.emplace_back(fd.get() >= 0 ? dup(fd.get()) : -1);
    }
    return out;
}

BaseBlock duplicateBaseBlock(const BaseBlock& src) {
    switch (src.getTag()) {
        case BaseBlock::nativeBlock:
            return BaseBlock::make<BaseBlock::nativeBlock>(
                    duplicateHandle(src.get<BaseBlock::nativeBlock>()));
        case BaseBlock::hwbBlock: {
            const HardwareBuffer& source = src.get<BaseBlock::hwbBlock>();
            HardwareBuffer copy;
            if (source) {
                AHardwareBuffer_acquire(source.get());
                copy.reset(source.get());
            }
            return BaseBlock::make<BaseBlock::hwbBlock>(std::move(copy));
        }
        case BaseBlock::pooledBlock:
            // Buffer-pool references require an IClientManager implementation.
            // Surface input uses hwbBlock on this guest.
            return BaseBlock();
    }
    return BaseBlock();
}

Buffer duplicateBuffer(const Buffer& src) {
    Buffer out;
    out.info = src.info;
    out.blocks.reserve(src.blocks.size());
    for (const auto& block : src.blocks) {
        aidl::android::hardware::media::c2::Block copy;
        copy.index = block.index;
        copy.meta = block.meta;
        copy.fence = duplicateHandle(block.fence);
        out.blocks.push_back(std::move(copy));
    }
    return out;
}

FrameData duplicateFrameData(const FrameData& src) {
    FrameData out;
    out.flags = src.flags;
    out.ordinal = src.ordinal;
    out.configUpdate = src.configUpdate;
    out.buffers.reserve(src.buffers.size());
    for (const auto& buffer : src.buffers) out.buffers.push_back(duplicateBuffer(buffer));
    out.infoBuffers.reserve(src.infoBuffers.size());
    for (const auto& info : src.infoBuffers) {
        aidl::android::hardware::media::c2::InfoBuffer copy;
        copy.index = info.index;
        copy.buffer = duplicateBuffer(info.buffer);
        out.infoBuffers.push_back(std::move(copy));
    }
    return out;
}

Work duplicateWork(const Work& src) {
    Work out;
    out.chainInfo = src.chainInfo;
    out.input = duplicateFrameData(src.input);
    out.worklets.reserve(src.worklets.size());
    for (const auto& worklet : src.worklets) {
        aidl::android::hardware::media::c2::Worklet copy;
        copy.componentId = worklet.componentId;
        copy.tunings = worklet.tunings;
        copy.failures = worklet.failures;
        copy.output = duplicateFrameData(worklet.output);
        out.worklets.push_back(std::move(copy));
    }
    out.workletsProcessed = src.workletsProcessed;
    out.result = src.result;
    return out;
}

}  // namespace gki
