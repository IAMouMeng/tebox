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

/* Listen on TCP host:port and accept one client (blocking accept in create
 * with listen backlog; send blocks briefly / drops if peer gone). */
struct cast_transport *cast_transport_listen(const char *host, int port);
struct cast_transport *cast_transport_connect(const char *host, int port);
void cast_transport_destroy(struct cast_transport *t);

bool cast_transport_ready(const struct cast_transport *t);
int cast_transport_fd(const struct cast_transport *t);

/* Non-blocking accept; returns 1 if client connected, 0 if none, -1 error. */
int cast_transport_try_accept(struct cast_transport *t);

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
