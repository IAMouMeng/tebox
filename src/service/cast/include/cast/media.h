/* Guest audio / camera / microphone over QEMU virtio-serial chardev sockets
 * (see CAST_CAMERA_* and the socket list in protocol.h). */
#ifndef TEBOX_CAST_MEDIA_H
#define TEBOX_CAST_MEDIA_H

#include "cast/protocol.h"

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct cast_media;

/* Socket paths live in dir (the directory of cast.sock). Never fails for
 * missing sockets: QEMU may start later, so connects are retried. */
struct cast_media *cast_media_open(const char *dir);
void cast_media_close(struct cast_media *m);

/* Called for every encoded AAC record (one ADTS frame). */
typedef void (*cast_media_audio_cb)(void *user, const uint8_t *adts, size_t size,
                                    uint64_t pts_us);

/* Non-blocking: (re)connects, drains guest audio and encodes it when
 * want_audio is set (somebody is watching), otherwise discards it. */
void cast_media_poll(struct cast_media *m, int want_audio,
                     cast_media_audio_cb cb, void *user);

/* Viewer uplink: a camera JPEG or microphone PCM (s16le 48 kHz mono). */
void cast_media_camera_jpeg(struct cast_media *m, const uint8_t *jpeg, size_t size);
void cast_media_mic_pcm(struct cast_media *m, const uint8_t *pcm, size_t size);

#ifdef __cplusplus
}
#endif

#endif
