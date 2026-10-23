/*
 * In-process API: tebox-v4l2 asks virtio-gpu-rutabaga to pull an RGBA frame
 * out of a virtio-gpu resource (same path as TEBOX_RES_SOCK / tebox-cast).
 * Returns 0 on success, negative errno otherwise. Weak-stubbed when the GPU
 * backend is not rutabaga.
 */
#ifndef TEBOX_GPU_RES_H
#define TEBOX_GPU_RES_H

#include <stdint.h>
#include <stddef.h>

int tebox_gpu_resource_read_rgba(uint32_t resource_id, uint32_t width,
                                 uint32_t height, void *dst, size_t dst_bytes,
                                 uint32_t *stride_out);

#endif
