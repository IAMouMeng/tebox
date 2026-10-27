#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace gki {

using VdecHandler = std::function<void(uint32_t type, const uint8_t* payload, size_t size)>;

/* gfxstream + can open tebox.vdec (probe; does not keep the port). */
bool vdecAvailable();

/* Ensure shared port + reader thread. Returns false if unavailable. */
bool vdecEnsure();

/* Allocate session id; register handler for PICTURE/FLUSHED for that session. */
uint32_t vdecAllocSession(VdecHandler handler);
void vdecFreeSession(uint32_t session);

bool vdecSend(uint32_t type, const void* a, uint32_t asize, const void* b = nullptr,
              uint32_t bsize = 0);

}  // namespace gki
