#ifndef TEBOX_CAST_CONTROL_H
#define TEBOX_CAST_CONTROL_H

#include "cast/encoder.h"

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cast_control;

typedef void (*cast_control_on_set_fn)(void *user,
                                       const struct cast_encoder_cfg *cfg,
                                       bool force_idr_only);
typedef void (*cast_control_on_stats_fn)(void *user, char *out, size_t out_sz);

struct cast_control *cast_control_listen(const char *host, int port,
                                         cast_control_on_set_fn on_set,
                                         cast_control_on_stats_fn on_stats,
                                         void *user);
void cast_control_destroy(struct cast_control *c);

/* Poll for one line of JSON; invoke callback. Non-blocking. */
int cast_control_poll(struct cast_control *c);

void cast_control_reply(struct cast_control *c, const char *json_line);

#ifdef __cplusplus
}
#endif

#endif
