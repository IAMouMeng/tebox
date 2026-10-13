#include "cast/protocol.h"

#include <string.h>

const char *cast_codec_name(enum cast_codec c)
{
    switch (c) {
    case CAST_CODEC_H264: return "h264";
    case CAST_CODEC_HEVC: return "hevc";
    case CAST_CODEC_VP8:  return "vp8";
    case CAST_CODEC_VP9:  return "vp9";
    case CAST_CODEC_AV1:  return "av1";
    default: return "unknown";
    }
}

enum cast_codec cast_codec_from_name(const char *name)
{
    if (!name) {
        return CAST_CODEC_H264;
    }
    if (!strcmp(name, "h264") || !strcmp(name, "avc")) {
        return CAST_CODEC_H264;
    }
    if (!strcmp(name, "hevc") || !strcmp(name, "h265")) {
        return CAST_CODEC_HEVC;
    }
    if (!strcmp(name, "vp8")) {
        return CAST_CODEC_VP8;
    }
    if (!strcmp(name, "vp9")) {
        return CAST_CODEC_VP9;
    }
    if (!strcmp(name, "av1")) {
        return CAST_CODEC_AV1;
    }
    return CAST_CODEC_H264;
}
