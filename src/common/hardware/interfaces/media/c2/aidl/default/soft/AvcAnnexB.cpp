#include "AvcAnnexB.h"

namespace gki {

bool avcHasIdr(const std::vector<uint8_t>& annexB) {
    for (size_t i = 0; i + 3 < annexB.size(); ++i) {
        if (annexB[i] == 0 && annexB[i + 1] == 0 && annexB[i + 2] == 1) {
            if ((annexB[i + 3] & 0x1f) == 5) return true;
            i += 2;
        }
    }
    return false;
}

std::vector<uint8_t> extractAvcCsd(const std::vector<uint8_t>& annexB) {
    std::vector<size_t> starts;
    for (size_t i = 0; i + 4 <= annexB.size();) {
        size_t n = 0;
        if (i + 4 <= annexB.size() && annexB[i] == 0 && annexB[i + 1] == 0 &&
            annexB[i + 2] == 0 && annexB[i + 3] == 1) {
            n = 4;
        } else if (i + 3 <= annexB.size() && annexB[i] == 0 && annexB[i + 1] == 0 &&
                   annexB[i + 2] == 1) {
            n = 3;
        }
        if (n) {
            starts.push_back(i);
            i += n;
        } else {
            ++i;
        }
    }
    if (starts.size() < 2) return {};
    size_t sps = starts[0];
    size_t ppsEnd = starts.size() > 2 ? starts[2] : annexB.size();
    if (sps >= ppsEnd || ppsEnd > annexB.size()) return {};
    const uint8_t* p = annexB.data() + sps;
    size_t pfx = (p + 4 <= annexB.data() + annexB.size() && p[2] == 0 && p[3] == 1) ? 4 : 3;
    if (sps + pfx >= annexB.size()) return {};
    uint8_t firstType = annexB[sps + pfx] & 0x1f;
    uint8_t secondType =
            annexB[starts[1] +
                   ((starts[1] + 4 <= annexB.size() && annexB[starts[1] + 2] == 0 &&
                     annexB[starts[1] + 3] == 1)
                            ? 4
                            : 3)] &
            0x1f;
    if (firstType != 7 || secondType != 8) return {};
    return std::vector<uint8_t>(annexB.begin() + sps, annexB.begin() + ppsEnd);
}

}  // namespace gki
