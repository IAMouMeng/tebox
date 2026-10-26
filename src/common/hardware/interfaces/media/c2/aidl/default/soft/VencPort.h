#pragma once

#include <cstdint>
#include <vector>

namespace gki {
namespace mesa_video {

bool isGfxstreamGpu();
/* Probe tebox.venc without leaving the port open (HAL registration). */
bool vencPortPresent();

/* Open host encode session: allocate session id, send CONFIG. */
bool vencOpen(uint32_t width, uint32_t height, uint32_t bitrate, uint32_t fps,
              uint32_t gopSec, uint32_t castCodec, int* outFd, uint32_t* outSession);

void vencClose(int fd, uint32_t session);

bool vencSubmitFrame(int fd, uint32_t session, uint64_t index, int64_t ptsUs,
                     uint32_t resourceId, uint32_t width, uint32_t height,
                     uint32_t format, bool keyframe);

/* Wait for bitstream reply with index >= wantIndex; status or coded bytes. */
int vencCollect(uint32_t session, uint64_t wantIndex, std::vector<uint8_t>* outBs);

}  // namespace mesa_video
}  // namespace gki
