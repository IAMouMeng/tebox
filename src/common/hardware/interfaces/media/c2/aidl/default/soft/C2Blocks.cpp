#include "C2Blocks.h"

#include "C2ParamBlob.h"
#include "FdIo.h"
#include "dma-heap.h"

#include <aidl/android/hardware/common/NativeHandle.h>
#include <android/log.h>
#include <cutils/ashmem.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <unistd.h>

#include <cstring>

#undef LOG_TAG
#define LOG_TAG "c2-blocks"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

using aidl::android::hardware::common::NativeHandle;
using aidl::android::hardware::media::c2::BaseBlock;
using aidl::android::hardware::media::c2::Block;
using aidl::android::hardware::media::c2::Buffer;
using aidl::android::hardware::media::c2::FrameData;

namespace gki {

bool makeOutputBlock(const std::vector<uint8_t>& data, std::vector<BaseBlock>* baseBlocks,
                     FrameData* output) {
    if (data.empty() || !baseBlocks || !output) return false;
    int heap = open("/dev/dma_heap/system", O_RDONLY | O_CLOEXEC);
    int fd = -1;
    if (heap >= 0) {
        dma_heap_allocation_data alloc = {};
        alloc.len = data.size();
        alloc.fd_flags = O_RDWR | O_CLOEXEC;
        if (ioctl(heap, DMA_HEAP_IOCTL_ALLOC, &alloc) == 0) fd = static_cast<int>(alloc.fd);
        close(heap);
    }
    if (fd < 0) {
        fd = ashmem_create_region("c2-output", data.size());
        if (fd >= 0) ashmem_set_prot_region(fd, PROT_READ | PROT_WRITE);
    }
    if (fd < 0) {
        ALOGE("output allocation failed size=%zu errno=%d", data.size(), errno);
        return false;
    }
    void* mapped = mmap(nullptr, data.size(), PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mapped == MAP_FAILED) {
        ALOGE("output mmap failed size=%zu errno=%d", data.size(), errno);
        close(fd);
        return false;
    }
    memcpy(mapped, data.data(), data.size());
    munmap(mapped, data.size());
    lseek(fd, 0, SEEK_SET);

    NativeHandle handle;
    handle.fds.emplace_back(fd);
    constexpr int32_t kC2IoMagic = '\xc2io\x00';
    handle.ints = {static_cast<int32_t>(data.size()), 0, kC2IoMagic};
    BaseBlock block = BaseBlock::make<BaseBlock::nativeBlock>(std::move(handle));
    const int32_t index = static_cast<int32_t>(baseBlocks->size());
    baseBlocks->push_back(std::move(block));

    Buffer buffer;
    aidl::android::hardware::media::c2::Block ref;
    ref.index = index;
    ref.meta.params.resize(16, 0);
    const uint32_t rangeSize = 16;
    const uint32_t rangeIndex = 0xE0000000u;
    const uint32_t zeroOffset = 0;
    const uint32_t rangeLength = static_cast<uint32_t>(data.size());
    memcpy(ref.meta.params.data(), &rangeSize, sizeof(rangeSize));
    memcpy(ref.meta.params.data() + 4, &rangeIndex, sizeof(rangeIndex));
    memcpy(ref.meta.params.data() + 8, &zeroOffset, sizeof(zeroOffset));
    memcpy(ref.meta.params.data() + 12, &rangeLength, sizeof(rangeLength));
    buffer.blocks.push_back(std::move(ref));
    output->buffers.push_back(std::move(buffer));
    return true;
}

std::vector<uint8_t> readLinearBlock(const BaseBlock& block, size_t maxBytes) {
    if (block.getTag() != BaseBlock::nativeBlock) return {};
    const NativeHandle& nh = block.get<BaseBlock::nativeBlock>();
    if (nh.fds.empty()) return {};
    if (nh.ints.size() >= 1 && nh.ints[0] > 0) {
        const size_t len = static_cast<size_t>(nh.ints[0]);
        if (len > maxBytes) return {};
        std::vector<uint8_t> out(len);
        size_t got = 0;
        int fd = nh.fds[0].get();
        while (got < len) {
            ssize_t n = pread(fd, out.data() + got, len - got, static_cast<off_t>(got));
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
        out.resize(got);
        if (!out.empty()) return out;
    }
    return readFd(nh.fds[0].get(), maxBytes);
}

bool readInputFrame(const FrameData& frame, const std::vector<BaseBlock>& baseBlocks,
                    std::vector<uint8_t>* out) {
    if (!out) return false;
    for (const Buffer& buffer : frame.buffers) {
        for (const Block& block : buffer.blocks) {
            if (block.index < 0 || static_cast<size_t>(block.index) >= baseBlocks.size()) continue;
            const BaseBlock& base = baseBlocks[static_cast<size_t>(block.index)];
            if (base.getTag() != BaseBlock::nativeBlock) {
                ALOGE("input block type %d not supported", static_cast<int>(base.getTag()));
                return false;
            }
            const NativeHandle& nh = base.get<BaseBlock::nativeBlock>();
            if (nh.fds.empty()) return false;
            struct {
                uint32_t offset, length;
            } range = {0, 0};
            if (!findParam(block.meta.params, kRangeInfoIndex, &range, sizeof(range))) {
                off_t end = lseek(nh.fds[0].get(), 0, SEEK_END);
                range.length = end > 0 ? static_cast<uint32_t>(end) : 0;
            }
            if (!range.length) continue;
            const size_t mapLen = static_cast<size_t>(range.offset) + range.length;
            void* map = mmap(nullptr, mapLen, PROT_READ, MAP_SHARED, nh.fds[0].get(), 0);
            if (map == MAP_FAILED) return false;
            const uint8_t* p = static_cast<const uint8_t*>(map) + range.offset;
            out->insert(out->end(), p, p + range.length);
            munmap(map, mapLen);
        }
    }
    return true;
}

}  // namespace gki

