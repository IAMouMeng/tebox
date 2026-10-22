/* SPDX-License-Identifier: GPL-2.0 */
/*
 * tebox-mali guest<->host wire ABI. MUST stay byte-for-byte in sync with the
 * QEMU device's qemu/hw/display/tebox-mali-proto.h. Kernel-side copy.
 */
#ifndef TEBOX_MALI_ABI_H
#define TEBOX_MALI_ABI_H

#include <linux/types.h>

#define TEBOX_MALI_VIRTIO_ID 61

enum { TEBOX_MALI_VQ_CMD = 0, TEBOX_MALI_VQ_EVT = 1, TEBOX_MALI_VQ_MAX = 2 };

enum {
	TEBOX_MALI_EVT_READABLE = 1,	/* ctx's kbase fd readable (poll wakeup) */
	TEBOX_MALI_EVT_FENCE = 2,	/* KCPU fence |id| signaled              */
};

#define TEBOX_MALI_EVT_DATA 64

struct tebox_mali_event {
	__u32 type;
	__u32 ctx;
	__u64 id;
	__u32 data_len;
	__u32 reserved;
	__u8  data[TEBOX_MALI_EVT_DATA];
} __packed;

enum {
	TEBOX_MALI_OP_OPEN = 1,
	TEBOX_MALI_OP_CLOSE = 2,
	TEBOX_MALI_OP_IOCTL = 3,
	TEBOX_MALI_OP_MMAP = 4,
	TEBOX_MALI_OP_MUNMAP = 5,
	TEBOX_MALI_OP_IMPORT = 6,
	TEBOX_MALI_OP_READ = 7,
};

struct tebox_mali_read_req {
	__u32 max_len;
	__u32 reserved;
} __packed;

struct tebox_mali_import_req {
	__u64 flags;
	__u32 type;
	__u32 n_seg;
} __packed;

struct tebox_mali_import_seg {
	__u64 gpa;
	__u64 len;
} __packed;

struct tebox_mali_import_resp {
	__u64 gpu_va;
	__u64 va_pages;
} __packed;

struct tebox_mali_cmd_hdr {
	__u32 op;
	__u32 ctx;
	__u32 seq;
	__u32 len;
} __packed;

struct tebox_mali_cmd_resp {
	__u32 seq;
	__s32 result;
	__u32 len;
	__u32 reserved;
} __packed;

struct tebox_mali_open_resp {
	__u32 ctx;
} __packed;

struct tebox_mali_ioctl_req {
	__u32 cmd;
	__u32 size;
	__u32 n_ind;
	__u32 reserved;
} __packed;

enum {
	TEBOX_MALI_IND_IN = 1,
	TEBOX_MALI_IND_OUT = 2,
	TEBOX_MALI_IND_FENCE_OUT = 4,	/* base_fence: host fd -> fence id */
};

struct tebox_mali_ioctl_ind {
	__u32 field_off;	/* u64 pointer field offset in the parent  */
	__u32 len;
	__u32 dir;
	__u32 parent;		/* 0: ioctl struct; k: buffer of ind k-1  */
} __packed;

struct tebox_mali_mmap_req {
	__u64 pgoff;
	__u64 size;
} __packed;

struct tebox_mali_mmap_resp {
	__u64 hostmem_offset;
	__u64 size;
	__u64 gpu_va;		/* host mapping address == the GPU VA (SAME_VA) */
	__u32 host_prot;	/* valid iff TEBOX_MALI_MMAP_F_PROT             */
	__u32 flags;		/* TEBOX_MALI_MMAP_F_*                          */
	__u32 handle;		/* OP_MUNMAP key                                */
	__u32 reserved;
} __packed;

#define TEBOX_MALI_MMAP_F_PROT 1u

struct tebox_mali_munmap_req {
	__u32 handle;
	__u32 reserved;
} __packed;

#define TEBOX_MALI_HOSTMEM_BAR 4
#define TEBOX_MALI_SHM_ID_HOSTMEM 0

#endif /* TEBOX_MALI_ABI_H */
