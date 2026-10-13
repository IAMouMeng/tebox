#ifndef TEBOX_CAST_ENDIAN_IO_H
#define TEBOX_CAST_ENDIAN_IO_H

#include <stdint.h>
#include <string.h>

static inline void cast_w32be(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static inline void cast_w64be(uint8_t *p, uint64_t v)
{
    cast_w32be(p, (uint32_t)(v >> 32));
    cast_w32be(p + 4, (uint32_t)v);
}

static inline uint32_t cast_r32be(const uint8_t *p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
           ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

static inline uint64_t cast_r64be(const uint8_t *p)
{
    return ((uint64_t)cast_r32be(p) << 32) | cast_r32be(p + 4);
}

#endif
