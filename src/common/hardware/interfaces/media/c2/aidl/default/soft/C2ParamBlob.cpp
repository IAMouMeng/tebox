#include "C2ParamBlob.h"

namespace gki {

void appendParam(std::vector<uint8_t>* blob, uint32_t index, const void* payload, size_t size) {
    const uint32_t total = static_cast<uint32_t>(8 + size);
    const size_t at = blob->size();
    blob->resize(at + ((total + 7u) & ~7u), 0);
    memcpy(blob->data() + at, &total, 4);
    memcpy(blob->data() + at + 4, &index, 4);
    memcpy(blob->data() + at + 8, payload, size);
}

bool findParam(const std::vector<uint8_t>& blob, uint32_t index, void* out, size_t size) {
    for (size_t at = 0; at + 8 <= blob.size();) {
        uint32_t psize = 0, pindex = 0;
        memcpy(&psize, blob.data() + at, 4);
        memcpy(&pindex, blob.data() + at + 4, 4);
        if (psize < 8 || at + psize > blob.size()) break;
        if (pindex == index && psize >= 8 + size) {
            memcpy(out, blob.data() + at + 8, size);
            return true;
        }
        at += (psize + 7u) & ~7u;
    }
    return false;
}

}  // namespace gki
