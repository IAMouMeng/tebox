/* Host video encoder for the guest Codec2 HAL (gfxstream backend).
 * See CAST_VENC_* in protocol.h. */
#ifndef TEBOX_CAST_VENC_H
#define TEBOX_CAST_VENC_H

#ifdef __cplusplus
extern "C" {
#endif

struct cast_venc;

/* Serves venc.sock and reads frames through res.sock, both in |dir| (next to
 * cast.sock). Runs on its own thread; reconnects when QEMU restarts. */
struct cast_venc *cast_venc_start(const char *dir);
void cast_venc_stop(struct cast_venc *v);

#ifdef __cplusplus
}
#endif

#endif
