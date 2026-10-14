/* Direct V4L2 M2M H.264 encoder for RGBA/BGRA frames (Linux only). */
#ifndef TEBOX_CAST_V4L2ENC_H
#define TEBOX_CAST_V4L2ENC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct cast_v4l2;

/* NULL when no V4L2 H.264 encoder accepts the frame format and size.
 * |dmabuf_input|: frames come as dma-bufs (cast_v4l2_encode_dmabuf). */
struct cast_v4l2 *cast_v4l2_open(int width, int height, bool rgba, int fps,
                                 int64_t bitrate, int gop_frames, bool dmabuf_input,
                                 int stride);
/* Encodes one frame; *out stays valid until the next call. */
int cast_v4l2_encode(struct cast_v4l2 *e, const uint8_t *pixels, int stride,
                     bool force_idr, const uint8_t **out, size_t *out_size,
                     bool *key);
/* Same for a frame in a dma-buf laid out at cast_v4l2_input_stride(); the
 * encoder reads it in place (zero-copy). */
int cast_v4l2_encode_dmabuf(struct cast_v4l2 *e, int fd, size_t length, bool force_idr,
                            const uint8_t **out, size_t *out_size, bool *key);
int cast_v4l2_input_stride(const struct cast_v4l2 *e);
bool cast_v4l2_dmabuf_input(const struct cast_v4l2 *e);
void cast_v4l2_close(struct cast_v4l2 *e);
const char *cast_v4l2_device(const struct cast_v4l2 *e);

#endif
