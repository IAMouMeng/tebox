// SPDX-License-Identifier: GPL-2.0
/*
 * Minimal stub module named "mali_kbase" that only exposes the sysfs module
 * parameters the stock CIX Mali userspace (libGLES_mali) probes when it
 * enumerates the GPU for EGL/GLES. The real device /dev/mali0 is provided by
 * tebox_mali (the passthrough); the compute path (OpenCL/csf_cqs) just opens
 * /dev/mali0 and works, but libGLES_mali's EGL device enumeration also reads
 * /sys/module/mali_kbase/parameters/large_page_conf and treats a failure to
 * open it as "No mali devices found" (eglInitialize -> EGL_NOT_INITIALIZED).
 * This stub creates that sysfs node so EGL enumeration can proceed. It holds no
 * device logic and never touches the GPU.
 */
#include <linux/module.h>
#include <linux/moduleparam.h>

/* 0 = large pages disabled; the passthrough uses 4K pages. Read-only to match
 * how mali_kbase exposes it. */
static int large_page_conf;
module_param(large_page_conf, int, 0444);
MODULE_PARM_DESC(large_page_conf,
		 "stub value for libmali's GPU enumeration probe (0 = off)");

MODULE_LICENSE("GPL");
MODULE_AUTHOR("tebox");
MODULE_DESCRIPTION("mali_kbase sysfs stub for stock libmali EGL device enumeration");
