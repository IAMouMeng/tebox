/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 *
 * Test virtgpu_virgl.c buffer allocation using gtest.
 */

#include <cerrno>
#include <cstring>

#include <gtest/gtest.h>
#include <xf86drm.h>

#include "drv.h"
#include "util.h"

extern "C" {
#include "external/virgl_hw.h"
#include "external/virtgpu_drm.h"
}

extern "C" const struct backend backend_virtgpu;

namespace
{

constexpr int kFakeDrmFd = 42;

int resource_create_count = 0;
struct drm_virtgpu_resource_create last_resource_create = {};
uint32_t next_bo_handle = 1;

/*
 * Creates a virtgpu_virgl driver backed by the fake ioctls below, which
 * emulate a 3D-capable virtio-gpu device whose host renderer only samples
 * from R8, so that multi-planar formats take the emulated buffer path.
 */
struct driver *create_virgl_driver()
{
	resource_create_count = 0;
	memset(&last_resource_create, 0, sizeof(last_resource_create));
	return drv_create(kFakeDrmFd, &backend_virtgpu);
}

} // namespace

extern "C" int drmIoctl(int fd, unsigned long request, void *arg)
{
	switch (request) {
	case DRM_IOCTL_VIRTGPU_GETPARAM: {
		struct drm_virtgpu_getparam *get_param = (struct drm_virtgpu_getparam *)arg;
		if (get_param->param == VIRTGPU_PARAM_3D_FEATURES) {
			*(uint32_t *)(uintptr_t)get_param->value = 1;
			return 0;
		}
		errno = EINVAL;
		return -1;
	}
	case DRM_IOCTL_VIRTGPU_GET_CAPS: {
		struct drm_virtgpu_get_caps *get_caps = (struct drm_virtgpu_get_caps *)arg;
		union virgl_caps *caps = (union virgl_caps *)(uintptr_t)get_caps->addr;
		memset(caps, 0, get_caps->size);
		caps->max_version = 1;
		caps->v1.sampler.bitmask[VIRGL_FORMAT_R8_UNORM / 32] |=
		    1u << (VIRGL_FORMAT_R8_UNORM % 32);
		return 0;
	}
	case DRM_IOCTL_VIRTGPU_RESOURCE_CREATE: {
		struct drm_virtgpu_resource_create *create =
		    (struct drm_virtgpu_resource_create *)arg;
		resource_create_count++;
		last_resource_create = *create;
		create->bo_handle = next_bo_handle++;
		create->res_handle = create->bo_handle;
		return 0;
	}
	case DRM_IOCTL_GEM_CLOSE:
		return 0;
	default:
		errno = EINVAL;
		return -1;
	}
}

TEST(virtgpu_virgl_unit_test, emulated_nv12_layout)
{
	struct driver *drv = create_virgl_driver();
	ASSERT_TRUE(drv);

	struct bo *bo = drv_bo_create(drv, 320, 240, DRM_FORMAT_NV12, BO_USE_TEXTURE);
	ASSERT_TRUE(bo);

	EXPECT_EQ(drv_bo_get_num_planes(bo), 2u);
	EXPECT_EQ(drv_bo_get_width(bo), 320u);
	EXPECT_EQ(drv_bo_get_height(bo), 240u);
	EXPECT_EQ(drv_bo_get_plane_stride(bo, 0), 320u);
	EXPECT_EQ(drv_bo_get_plane_stride(bo, 1), 320u);
	EXPECT_EQ(drv_bo_get_plane_offset(bo, 0), 0u);
	EXPECT_EQ(drv_bo_get_plane_offset(bo, 1), 320u * 240u);
	EXPECT_EQ(drv_bo_get_plane_size(bo, 0), 320u * 240u);
	EXPECT_EQ(drv_bo_get_plane_size(bo, 1), 320u * 120u);
	EXPECT_EQ(drv_bo_get_total_size(bo), 320u * 360u);

	EXPECT_EQ(resource_create_count, 1);
	EXPECT_EQ(last_resource_create.width, 320u);
	EXPECT_EQ(last_resource_create.height, 360u);
	EXPECT_EQ(last_resource_create.size, ALIGN(320u * 360u, 4096u));

	drv_bo_destroy(bo);
	drv_destroy(drv);
}

TEST(virtgpu_virgl_unit_test, emulated_yvu420_layout)
{
	struct driver *drv = create_virgl_driver();
	ASSERT_TRUE(drv);

	struct bo *bo = drv_bo_create(drv, 320, 240, DRM_FORMAT_YVU420, BO_USE_TEXTURE);
	ASSERT_TRUE(bo);

	EXPECT_EQ(drv_bo_get_num_planes(bo), 3u);
	EXPECT_EQ(drv_bo_get_plane_stride(bo, 0), 320u);
	EXPECT_EQ(drv_bo_get_plane_offset(bo, 1), 320u * 240u);
	EXPECT_EQ(drv_bo_get_plane_offset(bo, 2), 320u * 360u);
	EXPECT_EQ(drv_bo_get_plane_size(bo, 1), 320u * 120u);
	EXPECT_EQ(drv_bo_get_plane_size(bo, 2), 320u * 120u);
	EXPECT_EQ(drv_bo_get_total_size(bo), 320u * 480u);

	EXPECT_EQ(resource_create_count, 1);
	EXPECT_EQ(last_resource_create.width, 320u);
	EXPECT_EQ(last_resource_create.height, 480u);
	EXPECT_EQ(last_resource_create.size, ALIGN(320u * 480u, 4096u));

	drv_bo_destroy(bo);
	drv_destroy(drv);
}

TEST(virtgpu_virgl_unit_test, emulated_nv12_largest_supported_size)
{
	struct driver *drv = create_virgl_driver();
	ASSERT_TRUE(drv);

	/* The emulated R8 buffer is 65536x65535, just below 4GiB. */
	struct bo *bo = drv_bo_create(drv, 65536, 43690, DRM_FORMAT_NV12, BO_USE_TEXTURE);
	ASSERT_TRUE(bo);

	EXPECT_EQ(drv_bo_get_plane_stride(bo, 0), 65536u);
	EXPECT_EQ(drv_bo_get_total_size(bo), (size_t)65536 * 65535);

	EXPECT_EQ(resource_create_count, 1);
	EXPECT_EQ(last_resource_create.height, 65535u);
	EXPECT_EQ(last_resource_create.size, 65536u * 65535u);

	drv_bo_destroy(bo);
	drv_destroy(drv);
}

TEST(virtgpu_virgl_unit_test, emulated_nv12_too_large)
{
	struct driver *drv = create_virgl_driver();
	ASSERT_TRUE(drv);

	/* The emulated R8 buffer is 65536x65537, which exceeds 32 bits. */
	struct bo *bo = drv_bo_create(drv, 65536, 43691, DRM_FORMAT_NV12, BO_USE_TEXTURE);
	EXPECT_FALSE(bo);
	EXPECT_EQ(resource_create_count, 0);

	if (bo)
		drv_bo_destroy(bo);
	drv_destroy(drv);
}

TEST(virtgpu_virgl_unit_test, emulated_yvu420_too_large)
{
	struct driver *drv = create_virgl_driver();
	ASSERT_TRUE(drv);

	/* The emulated R8 buffer is 65536x87383, which exceeds 32 bits. */
	struct bo *bo = drv_bo_create(drv, 65536, 43691, DRM_FORMAT_YVU420, BO_USE_TEXTURE);
	EXPECT_FALSE(bo);
	EXPECT_EQ(resource_create_count, 0);

	if (bo)
		drv_bo_destroy(bo);
	drv_destroy(drv);
}

TEST(virtgpu_virgl_unit_test, emulated_nv12_height_too_large)
{
	struct driver *drv = create_virgl_driver();
	ASSERT_TRUE(drv);

	/* The emulated buffer height 2863311531 + 1431655766 exceeds 32 bits. */
	struct bo *bo = drv_bo_create(drv, 16, 2863311531u, DRM_FORMAT_NV12, BO_USE_TEXTURE);
	EXPECT_FALSE(bo);
	EXPECT_EQ(resource_create_count, 0);

	if (bo)
		drv_bo_destroy(bo);
	drv_destroy(drv);
}

TEST(virtgpu_virgl_unit_test, emulated_nv12_aligned_size_too_large)
{
	struct driver *drv = create_virgl_driver();
	ASSERT_TRUE(drv);

	/*
	 * The emulated R8 buffer is 1431655681x3 = 4294967043 bytes, which fits
	 * in 32 bits, but its page-aligned size does not.
	 */
	struct bo *bo = drv_bo_create(drv, 1431655681, 2, DRM_FORMAT_NV12, BO_USE_TEXTURE);
	EXPECT_FALSE(bo);
	EXPECT_EQ(resource_create_count, 0);

	if (bo)
		drv_bo_destroy(bo);
	drv_destroy(drv);
}

TEST(virtgpu_virgl_unit_test, use_flags_bind_mapping)
{
	struct driver *drv = create_virgl_driver();
	ASSERT_TRUE(drv);

	/* Test protected buffer bind flags (0xfu << 28) */
	struct bo *bo_protected = drv_bo_create(drv, 64, 64, DRM_FORMAT_NV12,
						BO_USE_TEXTURE | BO_USE_PROTECTED);
	ASSERT_TRUE(bo_protected);
	EXPECT_EQ(last_resource_create.bind & VIRGL_BIND_MINIGBM_PROTECTED,
		  VIRGL_BIND_MINIGBM_PROTECTED);
	drv_bo_destroy(bo_protected);

	/* Test software read/write flags including VIRGL_BIND_MINIGBM_SW_WRITE_RARELY (1u << 31) */
	struct bo *bo_sw = drv_bo_create(drv, 64, 64, DRM_FORMAT_NV12,
					 BO_USE_TEXTURE | BO_USE_SW_READ_RARELY |
					 BO_USE_SW_WRITE_RARELY);
	ASSERT_TRUE(bo_sw);
	EXPECT_EQ(last_resource_create.bind & VIRGL_BIND_MINIGBM_SW_WRITE_RARELY,
		  VIRGL_BIND_MINIGBM_SW_WRITE_RARELY);
	EXPECT_EQ(last_resource_create.bind & VIRGL_BIND_MINIGBM_SW_READ_RARELY,
		  VIRGL_BIND_MINIGBM_SW_READ_RARELY);
	drv_bo_destroy(bo_sw);

	drv_destroy(drv);
}

