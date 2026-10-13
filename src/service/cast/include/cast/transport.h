#ifndef TEBOX_CAST_TRANSPORT_H
#define TEBOX_CAST_TRANSPORT_H

#include "cast/protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cast_transport;

/* Listen on TCP host:port. Any number of viewers may connect; every packet
 * goes to all of them through per-viewer non-blocking buffers, so a slow
 * viewer is dropped (it can reconnect) instead of stalling the others. A new
 * viewer receives nothing until the next keyframe. */
struct cast_transport *cast_transport_listen(const char *host, int port);
struct cast_transport *cast_transport_connect(const char *host, int port);
void cast_transport_destroy(struct cast_transport *t);

bool cast_transport_ready(const struct cast_transport *t);

/* Non-blocking accept and housekeeping: drops viewers that disconnected and
 * flushes pending output. Returns 2 if a viewer was just accepted (send an
 * IDR), 1 if viewers remain connected, 0 if none, -1 on error. */
int cast_transport_try_accept(struct cast_transport *t);

/* Records viewers send upstream (camera JPEG, microphone PCM; see
 * protocol.h), delivered from cast_transport_try_accept. */
typedef void (*cast_uplink_cb)(void *user, uint8_t codec, const uint8_t *payload,
                               size_t size);
void cast_transport_set_uplink(struct cast_transport *t, cast_uplink_cb cb,
                               void *user);

int cast_transport_send_packet(struct cast_transport *t,
                               enum cast_codec codec, uint8_t flags,
                               uint64_t pts_us,
                               const uint8_t *payload, size_t size);

/* File sink for local Annex-B / raw bitstream verification. */
struct cast_file_sink;
struct cast_file_sink *cast_file_sink_open(const char *path);
void cast_file_sink_close(struct cast_file_sink *s);
int cast_file_sink_write(struct cast_file_sink *s,
                         const uint8_t *data, size_t size);

#ifdef __cplusplus
}
#endif

#endif
