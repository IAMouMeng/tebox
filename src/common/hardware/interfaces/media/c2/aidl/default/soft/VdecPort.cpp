#include "VdecPort.h"

#include "FdIo.h"
#include "VdecWire.h"
#include "VirtioSerial.h"

#include <android/log.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <cstring>
#include <map>
#include <mutex>
#include <thread>
#include <vector>

#undef LOG_TAG
#define LOG_TAG "tebox-vdec"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace gki {
namespace {

std::mutex gPortWrite;
std::mutex gPortMu;
int gPortFd = -1;
uint32_t gNextSession = 1;
std::map<uint32_t, VdecHandler> gHandlers;

bool isGfxstream() {
    char value[PROP_VALUE_MAX] = "";
    __system_property_get("ro.boot.tebox.gpu", value);
    return strcmp(value, "gfxstream") == 0;
}

// Hands every reply to its session. On a port error the open sessions fail
// (their pictures never come) and the HAL keeps the port closed.
void vdecReader(int fd) {
    std::vector<uint8_t> payload;
    for (;;) {
        VdecHdr hdr;
        if (!readFull(fd, &hdr, sizeof(hdr)) || hdr.magic != kVdecMagic ||
            hdr.size > (1u << 20) || hdr.size < sizeof(uint32_t))
            break;
        payload.resize(hdr.size);
        if (!readFull(fd, payload.data(), hdr.size)) break;
        uint32_t session;
        memcpy(&session, payload.data(), sizeof(session));
        VdecHandler handler;
        {
            std::lock_guard<std::mutex> state(gPortMu);
            auto it = gHandlers.find(session);
            if (it != gHandlers.end()) handler = it->second;
        }
        if (!handler) continue;  // closed meanwhile: a late reply
        if (hdr.type == kVdecPicture || hdr.type == kVdecFlushed)
            handler(hdr.type, payload.data(), payload.size());
    }
    ALOGE("tebox.vdec read failed");
    std::lock_guard<std::mutex> write(gPortWrite);
    std::lock_guard<std::mutex> state(gPortMu);
    gHandlers.clear();
    if (gPortFd == fd) gPortFd = -1;
    ::close(fd);
}

}  // namespace

bool vdecAvailable() {
    if (!isGfxstream()) return false;  // RGBA resources only there
    {
        std::lock_guard<std::mutex> state(gPortMu);
        if (gPortFd >= 0) return true;
    }
    const int fd = openVirtioPort("tebox.vdec");
    if (fd >= 0) ::close(fd);
    return fd >= 0;
}

bool vdecEnsure() {
    if (!isGfxstream()) return false;
    std::lock_guard<std::mutex> state(gPortMu);
    if (gPortFd >= 0) return true;
    gPortFd = openVirtioPort("tebox.vdec");
    if (gPortFd < 0) return false;
    std::thread(vdecReader, gPortFd).detach();
    return true;
}

uint32_t vdecAllocSession(VdecHandler handler) {
    std::lock_guard<std::mutex> state(gPortMu);
    const uint32_t session = gNextSession++;
    gHandlers[session] = std::move(handler);
    return session;
}

void vdecFreeSession(uint32_t session) {
    std::lock_guard<std::mutex> state(gPortMu);
    gHandlers.erase(session);
}

bool vdecSend(uint32_t type, const void* a, uint32_t asize, const void* b, uint32_t bsize) {
    std::lock_guard<std::mutex> lock(gPortWrite);
    int fd;
    {
        std::lock_guard<std::mutex> state(gPortMu);
        if (gPortFd < 0) return false;
        fd = gPortFd;
    }
    VdecHdr hdr = {kVdecMagic, type, asize + bsize};
    return writeFull(fd, &hdr, sizeof(hdr)) && (!asize || writeFull(fd, a, asize)) &&
           (!bsize || writeFull(fd, b, bsize));
}

}  // namespace gki
