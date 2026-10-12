/* Local tebox cast capture hook; not an upstream QEMU submission. */
#ifndef TEBOX_CAST_HOOK_H
#define TEBOX_CAST_HOOK_H

#include <stdbool.h>
#include <stdint.h>

/* Capture the guest scanout framebuffer (current GL context) for cast.
 * Shared by the SDL and egl-headless displays. */
void tebox_cast_capture(unsigned int framebuffer, int src_w, int src_h,
                        bool y0_top);
void tebox_cast_shutdown(void);

/* True when TEBOX_CAST=1. */
bool tebox_cast_enabled(void);

/* Target GUI update interval in ms (e.g. 16 for ~60 FPS). 0 = leave default. */
uint64_t tebox_cast_refresh_interval_ms(void);

#endif
