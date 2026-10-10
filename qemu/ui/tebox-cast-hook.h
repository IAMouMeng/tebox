/* Local tebox cast capture hook; not an upstream QEMU submission. */
#ifndef TEBOX_CAST_HOOK_H
#define TEBOX_CAST_HOOK_H

#include <stdbool.h>
#include <stdint.h>

struct sdl2_console;

void tebox_cast_on_scanout(struct sdl2_console *scon);
void tebox_cast_shutdown(struct sdl2_console *scon);

/* True when TEBOX_CAST=1. */
bool tebox_cast_enabled(void);

/* Target GUI update interval in ms (e.g. 16 for ~60 FPS). 0 = leave default. */
uint64_t tebox_cast_refresh_interval_ms(void);

#endif
