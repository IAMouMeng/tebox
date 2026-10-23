#pragma once
#include <cstdint>
typedef int32_t audio_port_handle_t;
typedef uint32_t audio_format_t;
#ifndef AUDIO_FORMAT_PCM_16_BIT
#define AUDIO_FORMAT_PCM_16_BIT 0x1u
#endif
#ifndef AUDIO_FORMAT_INVALID
#define AUDIO_FORMAT_INVALID 0xffffffffu
#endif
