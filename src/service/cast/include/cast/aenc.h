/* Host audio encoder for the guest Codec2 HAL (tebox.aenc). */
#ifndef TEBOX_CAST_AENC_H
#define TEBOX_CAST_AENC_H

#ifdef __cplusplus
extern "C" {
#endif

struct cast_aenc;

/* Serves aenc.sock in |dir| (next to cast.sock). Own thread; reconnects. */
struct cast_aenc *cast_aenc_start(const char *dir);
void cast_aenc_stop(struct cast_aenc *a);

#ifdef __cplusplus
}
#endif

#endif
