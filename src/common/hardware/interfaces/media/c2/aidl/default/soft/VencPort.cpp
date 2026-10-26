#include "VencPort.h"

#include "FdIo.h"
#include "VencWire.h"
#include "VirtioSerial.h"

#include <android/log.h>
#include <errno.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <cstring>
#include <thread>
#include <vector>

#define LOG_TAG "mesa-pipe-video"
#define ALOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)

namespace gki {
namespace mesa_video {
namespace {

// tebox.venc is a virtio-serial port with one opener at a time, while every
// encoder lives in this process: the port is opened once and shared. Each
// session has an id; one reader thread files the host's replies by session,
// so concurrent encoders (two screen-cast clients) do not disturb each other.
struct VencReply {
    uint64_t index;
    int32_t status;
    std::vector<uint8_t> data;
};
std::mutex gVencWrite;  // whole messages
std::mutex gVencMu;     // guards the state below
std::condition_variable gVencCv;
int gVencFd = -1;
uint32_t gVencNextSession = 1;
std::map<uint32_t, std::deque<VencReply>> gVencReplies;  // open sessions

// |fd| is the port the session was opened on: a reopened port (after an
// error) is not that session's any more, and a closed fd number may be reused.
bool vencSend(int fd, uint32_t type, const void* payload, uint32_t size) {
    VencHdr hdr = {kVencMagic, type, size};
    std::lock_guard<std::mutex> lock(gVencWrite);
    {
        std::lock_guard<std::mutex> state(gVencMu);
        if (fd < 0 || fd != gVencFd) return false;
    }
    return writeFull(fd, &hdr, sizeof(hdr)) && (!size || writeFull(fd, payload, size));
}

// Files every reply under its session. On a port error (QEMU or tebox-cast
// went away) every waiting session fails and the port is reopened later.
void vencReaderLoop(int fd) {
    for (;;) {
        VencHdr hdr;
        VencOut out;
        if (!readFull(fd, &hdr, sizeof(hdr)) || hdr.magic != kVencMagic ||
            hdr.type != kVencBitstream || hdr.size < sizeof(out) || hdr.size > (16u << 20) ||
            !readFull(fd, &out, sizeof(out)))
            break;
        VencReply reply = {out.index, out.status, std::vector<uint8_t>(hdr.size - sizeof(out))};
        if (!reply.data.empty() && !readFull(fd, reply.data.data(), reply.data.size())) break;
        std::lock_guard<std::mutex> lock(gVencMu);
        auto it = gVencReplies.find(out.session);
        if (it != gVencReplies.end()) it->second.push_back(std::move(reply));
        gVencCv.notify_all();
    }
    ALOGE("host encoder: tebox.venc read failed");
    std::lock_guard<std::mutex> write(gVencWrite);  // no send is using |fd|
    std::lock_guard<std::mutex> lock(gVencMu);
    for (auto& [session, replies] : gVencReplies) replies.push_back({UINT64_MAX, -EIO, {}});
    if (gVencFd == fd) gVencFd = -1;
    ::close(fd);
    gVencCv.notify_all();
}

// Opens the port (and its reader) if needed; returns the fd or -1.
int vencPortLocked() {
    if (gVencFd >= 0) return gVencFd;
    gVencFd = openVirtioPort("tebox.venc");
    if (gVencFd >= 0) std::thread(vencReaderLoop, gVencFd).detach();
    return gVencFd;
}

}  // namespace

bool isGfxstreamGpu() {
    char value[PROP_VALUE_MAX] = "";
    __system_property_get("ro.boot.tebox.gpu", value);
    return strcmp(value, "gfxstream") == 0;
}

bool vencPortPresent() {
    const int fd = openVirtioPort("tebox.venc");
    if (fd >= 0) ::close(fd);
    return fd >= 0;
}

bool vencOpen(uint32_t width, uint32_t height, uint32_t bitrate, uint32_t fps, uint32_t gopSec,
              uint32_t castCodec, int* outFd, uint32_t* outSession) {
    if (!outFd || !outSession) return false;
    int fd;
    uint32_t session;
    {
        std::lock_guard<std::mutex> lock(gVencMu);
        fd = vencPortLocked();
        session = gVencNextSession++;
        if (fd >= 0) gVencReplies[session];
    }
    const VencConfig cfg = {session, width, height, bitrate, fps, gopSec, castCodec};
    if (fd < 0 || !vencSend(fd, kVencConfig, &cfg, sizeof(cfg))) {
        std::lock_guard<std::mutex> lock(gVencMu);
        gVencReplies.erase(session);
        return false;
    }
    *outFd = fd;
    *outSession = session;
    return true;
}

void vencClose(int fd, uint32_t session) {
    // The port stays open for the other sessions.
    vencSend(fd, kVencClose, &session, sizeof(session));
    {
        std::lock_guard<std::mutex> lock(gVencMu);
        gVencReplies.erase(session);
    }
    gVencCv.notify_all();
}

bool vencSubmitFrame(int fd, uint32_t session, uint64_t index, int64_t ptsUs,
                     uint32_t resourceId, uint32_t width, uint32_t height, uint32_t format,
                     bool keyframe) {
    const VencFrame frame = {session, index, ptsUs, resourceId, width, height, format,
                             keyframe ? kVencFlagKey : 0u};
    return fd >= 0 && vencSend(fd, kVencFrame, &frame, sizeof(frame));
}

int vencCollect(uint32_t session, uint64_t wantIndex, std::vector<uint8_t>* outBs) {
    if (!outBs) return -EINVAL;
    outBs->clear();
    // Replies come one per frame, in submission order per session.
    std::unique_lock<std::mutex> lock(gVencMu);
    for (;;) {
        auto it = gVencReplies.find(session);
        if (it == gVencReplies.end()) return -EIO;  // closed meanwhile
        if (it->second.empty()) {
            if (!gVencCv.wait_for(lock, std::chrono::seconds(5), [&] {
                    auto q = gVencReplies.find(session);
                    return q == gVencReplies.end() || !q->second.empty();
                }))
                return -ETIMEDOUT;
            continue;
        }
        VencReply reply = std::move(it->second.front());
        it->second.pop_front();
        if (reply.index < wantIndex) continue;  // reply to an abandoned frame
        if (reply.status < 0) return reply.status;
        *outBs = std::move(reply.data);
        return static_cast<int>(outBs->size());
    }
}

}  // namespace mesa_video
}  // namespace gki
