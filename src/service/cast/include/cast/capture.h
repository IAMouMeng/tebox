#ifndef TEBOX_CAST_CAPTURE_H
#define TEBOX_CAST_CAPTURE_H

#include "cast/protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cast_capture;

struct cast_frame {
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint64_t pts_us;
    uint8_t *data;
    size_t size;
};

/* Listen on a Unix domain socket for QEMU tebox-cast-hook frames. */
struct cast_capture *cast_capture_listen_unix(const char *path);
void cast_capture_destroy(struct cast_capture *cap);

/* Non-blocking: 1 = frame, 0 = none, -1 = error/disconnect. */
int cast_capture_poll_frame(struct cast_capture *cap, struct cast_frame *out);
void cast_frame_free(struct cast_frame *f);

#ifdef __cplusplus
}
#endif

#endif
