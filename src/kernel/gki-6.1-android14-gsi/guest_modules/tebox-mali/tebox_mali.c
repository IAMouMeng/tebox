// SPDX-License-Identifier: GPL-2.0
/*
 * tebox-mali: guest shim for the QEMU tebox-mali virtio device. Presents
 * /dev/mali0 and passes the CIX Mali kbase UAPI through to the host's real
 * /dev/mali0 (Mali-G720, CSF, DDK r49p0), so the guest's Mali userspace (stock
 * CIX blob or open PanVK) drives the real GPU with no gfxstream/virgl layer.
 *
 * Control plane: generic kbase ioctl forward over the CMD virtqueue (the CSF
 * ioctls are flat, pointer-free structs). One open() == one host fd (context).
 *
 * Memory plane (zero-copy, SAME_VA): on mmap the host maps its fd at the same
 * cookie, exposes the pages inside a hostmem PCI BAR, and returns the window
 * offset; we remap the user VMA onto that BAR page. get_unmapped_area returns
 * the cookie for real GPU VAs so the userspace pointer equals the GPU VA.
 *
 * See tebox-mali-abi.h and qemu/hw/display/tebox-mali-proto.h.
 */
#include <linux/module.h>
#include <linux/miscdevice.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/completion.h>
#include <linux/scatterlist.h>
#include <linux/mm.h>
#include <linux/mman.h>
#include <linux/io.h>
#include <linux/uaccess.h>
#include <linux/dma-buf.h>
#include <linux/dma-fence.h>
#include <linux/sync_file.h>
#include <linux/anon_inodes.h>
#include <linux/file.h>
#include <linux/poll.h>
#include <linux/wait.h>
#include <linux/spinlock.h>
#include <linux/xarray.h>
#include <linux/hrtimer.h>
#include <linux/workqueue.h>
#include <linux/virtio.h>
#include <linux/virtio_config.h>
#include <linux/kprobes.h>

#include "tebox-mali-abi.h"

/* do_munmap is not EXPORT_SYMBOL'd on GKI; resolve once via kprobe so
 * get_unmapped_area can clear SAME_VA collisions while mmap_lock is held
 * (vm_munmap would re-take the lock and deadlock). */
typedef int (*tbx_do_munmap_fn)(struct mm_struct *mm, unsigned long addr,
				size_t len, struct list_head *uf);
static tbx_do_munmap_fn tbx_do_munmap;

static int tbx_resolve_do_munmap(void)
{
	struct kprobe kp = { .symbol_name = "do_munmap" };
	int ret;

	ret = register_kprobe(&kp);
	if (ret)
		return ret;
	tbx_do_munmap = (tbx_do_munmap_fn)kp.addr;
	unregister_kprobe(&kp);
	return tbx_do_munmap ? 0 : -ENOENT;
}

/* Hot-path tracing. MEM_IMPORT fires per client buffer per composite under
 * egl=mali (SurfaceFlinger/RenderEngine), so an unconditional pr_info per import
 * is thousands of console printks/sec and pegs a vCPU — the same lesson the host
 * side learned (TEBOX_MALI_TRACE). Off by default; enable with
 * `insmod tebox_mali.ko debug=1` or `echo 1 > /sys/module/tebox_mali/parameters/debug`. */
static bool tbx_debug;
module_param_named(debug, tbx_debug, bool, 0644);
MODULE_PARM_DESC(debug,
		 "log open/ioctl/mmap/munmap calls (chatty; default off; "
		 "echo 1 > /sys/module/tebox_mali/parameters/debug)");

/* Bring-up op counters, read via /sys/module/tebox_mali/parameters/ (no log
 * flooding). c_mmap/c_munmap each drive one host memory_region add/del (the
 * suspected swipe-time flatview-rebuild CPU cost); c_ioctl is a sync vq
 * round-trip. Sample the delta across a swipe to size the real hot path. */
static int tbx_c_ioctl, tbx_c_mmap, tbx_c_munmap;
module_param_named(c_ioctl, tbx_c_ioctl, int, 0444);
module_param_named(c_mmap, tbx_c_mmap, int, 0444);
module_param_named(c_munmap, tbx_c_munmap, int, 0444);
/* Batch CS_EVENT_SIGNAL: fire_flush_us caps deferral (count-only hung CSF). */
static int tbx_fire_batch = 32;
module_param_named(fire_batch, tbx_fire_batch, int, 0644);
static int tbx_fire_flush_us = 100;
module_param_named(fire_flush_us, tbx_fire_flush_us, int, 0644);
/* Coalesce MMAP/MUNMAP kicks. 0 = kick every mmap. */
static int tbx_mmap_flush_us = 200;
module_param_named(mmap_flush_us, tbx_mmap_flush_us, int, 0644);
/* Default off: host kick_timer already SIGNAL+QUEUE_KICKs. */
static int tbx_cs_poll_us;
module_param_named(cs_poll_us, tbx_cs_poll_us, int, 0644);
#define tbx_dbg(fmt, ...) \
	do { if (tbx_debug) pr_info("tebox-mali: " fmt, ##__VA_ARGS__); } while (0)

#define TBX_CMD_BUF (32 * 1024)	/* holds struct + indirect GPUPROPS/GLB data */
#define TBX_CMD_SLOTS 16	/* concurrent CMD requests (fire-and-forget + sync) */
#define TBX_EV_Q 32		/* piggybacked kbase records per open (host drains) */
#define TBX_MAP_CACHE 1024	/* per-open mmap cache; CSF apps map many buffers
				 * (csf_cqs ~5/iteration), 64 exhausted at ~iter 14 */
#define TBX_MAX_IND 64		/* == host TEBOX_MALI_MAX_IND */
#define TBX_MAX_FLAT_IND 4	/* tbx_ioctl_indirects() output */
#define TBX_MAX_CTX 64		/* == host TEBOX_MALI_MAX_CTX */
#define TBX_EVT_BUFS 64

/* kbase ioctl identifiers we must expand with indirect-pointer handling. */
#define KBASE_IOCTL_TYPE 0x80
#define KBASE_NR_GET_GPUPROPS 3
#define KBASE_NR_MEM_ALLOC 5
#define KBASE_NR_MEM_ALLOC_EX 59
#define KBASE_NR_MEM_FREE 7
#define KBASE_NR_CS_GET_GLB_IFACE 51
#define KBASE_NR_MEM_IMPORT 22
#define KBASE_NR_STREAM_CREATE 24
#define KBASE_NR_FENCE_VALIDATE 25
#define KBASE_NR_STICKY_RESOURCE_MAP 29
#define KBASE_NR_STICKY_RESOURCE_UNMAP 30
#define KBASE_NR_KCPU_QUEUE_ENQUEUE 47
#define KBASE_NR_CS_QUEUE_KICK 37
#define KBASE_NR_CS_EVENT_SIGNAL 44
#define KBASE_NR_QUEUE_TERMINATE 41
#define KBASE_NR_GROUP_TERMINATE 43

static const char *tbx_nr_name(unsigned int nr)
{
	switch (nr) {
	case KBASE_NR_GET_GPUPROPS:		return "GET_GPUPROPS";
	case KBASE_NR_MEM_ALLOC:		return "MEM_ALLOC";
	case KBASE_NR_MEM_FREE:			return "MEM_FREE";
	case KBASE_NR_MEM_IMPORT:		return "MEM_IMPORT";
	case KBASE_NR_STREAM_CREATE:		return "STREAM_CREATE";
	case KBASE_NR_FENCE_VALIDATE:		return "FENCE_VALIDATE";
	case KBASE_NR_STICKY_RESOURCE_MAP:	return "STICKY_MAP";
	case KBASE_NR_STICKY_RESOURCE_UNMAP:	return "STICKY_UNMAP";
	case KBASE_NR_CS_QUEUE_KICK:		return "QUEUE_KICK";
	case KBASE_NR_QUEUE_TERMINATE:		return "QUEUE_TERMINATE";
	case KBASE_NR_GROUP_TERMINATE:		return "GROUP_TERMINATE";
	case KBASE_NR_CS_EVENT_SIGNAL:		return "CS_EVENT_SIGNAL";
	case KBASE_NR_KCPU_QUEUE_ENQUEUE:	return "KCPU_ENQUEUE";
	case KBASE_NR_CS_GET_GLB_IFACE:		return "CS_GET_GLB_IFACE";
	case KBASE_NR_MEM_ALLOC_EX:		return "MEM_ALLOC_EX";
	default:				return NULL;
	}
}

/* CSF KCPU UAPI (mali_base_csf_kernel.h): struct base_kcpu_command is
 * {u8 type; u8 pad[7]; union info (16 bytes)}; info starts with the u64
 * pointer (if any) followed by its element count. */
#define TBX_KCPU_CMD_SIZE 24
#define TBX_KCPU_MAX_CMDS 1024
enum {
	TBX_KCPU_FENCE_SIGNAL = 0,
	TBX_KCPU_FENCE_WAIT,
	TBX_KCPU_CQS_WAIT,
	TBX_KCPU_CQS_SET,
	TBX_KCPU_CQS_WAIT_OPERATION,
	TBX_KCPU_CQS_SET_OPERATION,
	TBX_KCPU_MAP_IMPORT,
	TBX_KCPU_UNMAP_IMPORT,
	TBX_KCPU_UNMAP_IMPORT_FORCE,
	TBX_KCPU_JIT_ALLOC,
	TBX_KCPU_JIT_FREE,
	TBX_KCPU_GROUP_SUSPEND,
	TBX_KCPU_ERROR_BARRIER,
};
#define TBX_BASE_FENCE_SIZE 8		/* struct base_fence {int fd, stream_fd} */
#define TBX_JIT_ALLOC_INFO_SIZE 48	/* struct base_jit_alloc_info */
/* Passthrough adds virtio + host kbase latency; Unreal/Vulkan KCPU waits
 * routinely exceed 3s when a deferred doorbell or EVT is late. */
#define TBX_FENCE_WAIT_MS 15000
#define BASE_MEM_IMPORT_TYPE_UMM 2	/* dmabuf import */
#define TBX_MAX_SEG 256
/* SAME_VA mmap cookies: [BASE_MEM_COOKIE_BASE, +BITS_PER_LONG) pages. Only these
 * need CPU VA == GPU VA; special handles (tracking page, CSF USER_IO/USER_REG)
 * may live at any CPU address, as with native kbase. */
#define TBX_COOKIE_FIRST 0x40
#define TBX_COOKIE_LAST 0x7f
/* CSF USER_IO doorbell pages (BASEP_MEM_CSF_USER_IO_PAGES_HANDLE .. before
 * SAME_VA cookies). Must be write-combine like native kbase: a WB guest PTE
 * over the same PA as host WC leaves doorbell stores invisible to the MCU. */
#define TBX_USER_IO_FIRST 0x30
#define TBX_USER_IO_LAST  0x3f

/* One cached buffer mapping. kbase assigns a SAME_VA buffer's GPU VA at mmap()
 * time to wherever the HOST maps it, so we ask the host to map first (in
 * get_unmapped_area), learn the GPU VA, and place the user VMA at that exact
 * address so the guest CPU pointer equals the GPU VA. */
struct tebox_mali_map_ent {
	u64 cookie;	/* the mmap pgoff (kbase cookie / handle) */
	u64 gpu_va;	/* host map addr == GPU VA                */
	u64 hm_off;	/* byte offset in the hostmem BAR         */
	u64 size;
	u32 host_prot;	/* PROT_* of the host mapping             */
	u32 handle;	/* host OP_MUNMAP key                     */
	unsigned int users;	/* VMAs (incl. split pieces) over it */
	bool used;
	bool mapped;	/* .mmap consumed it; kbase recycles cookies */
};

struct tbx_cmd_slot {
	void *req;
	void *resp;
	struct completion done;
	bool used;
	bool fire;			/* no waiter; recycle in cmd_done */
};

struct tebox_mali_dev {
	struct virtio_device *vdev;
	struct virtqueue *cmd_vq;
	struct miscdevice misc;

	spinlock_t vq_lock;		/* virtqueue add/get/kick */
	struct tbx_cmd_slot slots[TBX_CMD_SLOTS];
	spinlock_t slot_lock;
	wait_queue_head_t slot_wq;
	u32 seq;

	phys_addr_t shm_base;		/* hostmem BAR guest-physical base */
	u64 shm_len;

	struct virtqueue *evt_vq;
	struct tebox_mali_event evt_bufs[TBX_EVT_BUFS];
	spinlock_t ctx_lock;		/* ctx_files[] vs. EVT_READABLE */
	struct tebox_mali_file *ctx_files[TBX_MAX_CTX];

	/* KCPU fences by host id: a tbx_fence, or xa_mk_value(1) when the host
	 * signaled before the ioctl that created it returned. */
	struct xarray fences;
	spinlock_t fence_lock;		/* dma_fence lock */
	u64 fence_ctx;
};

struct tebox_mali_file {
	struct tebox_mali_dev *td;
	u32 ctx;
	bool open_ok;
	bool readable;			/* host reported the kbase fd readable */
	wait_queue_head_t wq;
	struct mutex map_lock;
	struct tebox_mali_map_ent maps[TBX_MAP_CACHE];
	/* Coalesce OP_MUNMAP into OP_MUNMAP_BATCH across a free storm. */
	u32 munmap_pending[TEBOX_MALI_MUNMAP_BATCH_MAX];
	u32 munmap_n;
	/* EVT_READABLE payload, so read() usually skips a virtio OP_READ. */
	spinlock_t ev_lock;
	u8 ev_data[TBX_EV_Q][TEBOX_MALI_EVT_DATA];
	u16 ev_len[TBX_EV_Q];
	u8 ev_head, ev_n;
};

static void tbx_slot_release(struct tebox_mali_dev *td, struct tbx_cmd_slot *s)
{
	unsigned long flags;

	spin_lock_irqsave(&td->slot_lock, flags);
	s->used = false;
	s->fire = false;
	spin_unlock_irqrestore(&td->slot_lock, flags);
	wake_up(&td->slot_wq);
}

static struct tbx_cmd_slot *tbx_slot_get(struct tebox_mali_dev *td)
{
	struct tbx_cmd_slot *s = NULL;
	unsigned long flags;
	int i;

	for (;;) {
		spin_lock_irqsave(&td->slot_lock, flags);
		for (i = 0; i < TBX_CMD_SLOTS; i++) {
			if (!td->slots[i].used) {
				s = &td->slots[i];
				s->used = true;
				s->fire = false;
				break;
			}
		}
		spin_unlock_irqrestore(&td->slot_lock, flags);
		if (s)
			return s;
		if (wait_event_interruptible(td->slot_wq, ({
			int j, free = 0;
			spin_lock_irqsave(&td->slot_lock, flags);
			for (j = 0; j < TBX_CMD_SLOTS; j++)
				free |= !td->slots[j].used;
			spin_unlock_irqrestore(&td->slot_lock, flags);
			free;
		})))
			return NULL;
	}
}

static void tebox_mali_cmd_done(struct virtqueue *vq)
{
	struct tebox_mali_dev *td = vq->vdev->priv;
	struct tbx_cmd_slot *s, *fire[TBX_CMD_SLOTS];
	unsigned int len, nfire = 0;
	unsigned long flags;

	spin_lock_irqsave(&td->vq_lock, flags);
	while ((s = virtqueue_get_buf(vq, &len))) {
		if (s->fire && nfire < TBX_CMD_SLOTS)
			fire[nfire++] = s;
		else if (!s->fire)
			complete(&s->done);
	}
	spin_unlock_irqrestore(&td->vq_lock, flags);
	while (nfire)
		tbx_slot_release(td, fire[--nfire]);
}

/* ---- KCPU fences --------------------------------------------------------- */

static void tbx_fire_kick(struct tebox_mali_dev *td);

struct tbx_fence {
	struct dma_fence base;
};

static const char *tbx_fence_driver_name(struct dma_fence *f)
{
	return "tebox-mali";
}

static const char *tbx_fence_timeline_name(struct dma_fence *f)
{
	return "kcpu";
}

static const struct dma_fence_ops tbx_fence_ops = {
	.get_driver_name = tbx_fence_driver_name,
	.get_timeline_name = tbx_fence_timeline_name,
};

/* EVT_FENCE (irq context). */
static void tbx_fence_host_signaled(struct tebox_mali_dev *td, u32 id)
{
	unsigned long flags;
	void *e;

	xa_lock_irqsave(&td->fences, flags);
	e = xa_load(&td->fences, id);
	if (e && !xa_is_value(e))
		__xa_erase(&td->fences, id);
	else if (!e)
		__xa_store(&td->fences, id, xa_mk_value(1), GFP_ATOMIC);
	xa_unlock_irqrestore(&td->fences, flags);
	if (e && !xa_is_value(e)) {
		dma_fence_signal(e);
		dma_fence_put(e);
	}
}

/* Wrap host fence |id| in a guest sync_file; returns the new fd. */
static int tbx_fence_install(struct tebox_mali_dev *td, u32 id)
{
	struct tbx_fence *f;
	struct sync_file *sf;
	unsigned long flags;
	void *old;
	int fd;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	dma_fence_init(&f->base, &tbx_fence_ops, &td->fence_lock, td->fence_ctx,
		       id);
	sf = sync_file_create(&f->base);
	if (!sf) {
		dma_fence_put(&f->base);
		return -ENOMEM;
	}
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(sf->file);
		dma_fence_put(&f->base);
		return fd;
	}
	/* The xarray takes over our initial reference. */
	xa_lock_irqsave(&td->fences, flags);
	old = __xa_cmpxchg(&td->fences, id, NULL, &f->base, GFP_ATOMIC);
	if (xa_is_value(old))
		__xa_erase(&td->fences, id);
	xa_unlock_irqrestore(&td->fences, flags);
	if (old) {		/* already signaled, or no memory to track it */
		dma_fence_signal(&f->base);
		dma_fence_put(&f->base);
	}
	fd_install(fd, sf->file);
	return fd;
}

/* KCPU FENCE_WAIT on a guest sync_file: the host can't see guest fds, so wait
 * here, before the commands queued after it are submitted.
 * Flush deferred CS_EVENT_SIGNAL doorbells first — otherwise the GPU work that
 * should signal this fence may still be sitting on the avail ring un-kicked. */
static int tbx_fence_wait_user(struct tebox_mali_dev *td, const u8 *kcmd)
{
	struct dma_fence *f;
	u64 ptr;
	long t;
	int fd;

	tbx_fire_kick(td);
	memcpy(&ptr, kcmd + 8, 8);
	if (get_user(fd, (int __user *)(uintptr_t)ptr))
		return -EFAULT;
	if (fd < 0)
		return 0;
	f = sync_file_get_fence(fd);
	if (!f)
		return -EINVAL;
	t = dma_fence_wait_timeout(f, false, msecs_to_jiffies(TBX_FENCE_WAIT_MS));
	dma_fence_put(f);
	if (!t) {
		pr_warn_ratelimited("tebox-mali: KCPU fence wait timed out (%d ms)\n",
				    TBX_FENCE_WAIT_MS);
		return -ETIMEDOUT;
	}
	return t < 0 ? t : 0;
}

/* FENCE_VALIDATE names a guest sync_file the host kbase cannot see; kbase
 * only checks that the fd is a sync_file, so do that here. */
static long tbx_fence_validate(unsigned long arg)
{
	struct dma_fence *f;
	int fd;

	if (get_user(fd, (int __user *)arg))
		return -EFAULT;
	f = sync_file_get_fence(fd);
	if (!f)
		return -EINVAL;
	dma_fence_put(f);
	return 0;
}

/* ---- EVT queue ----------------------------------------------------------- */

static void tbx_evt_push(struct tebox_mali_file *tf, const u8 *data, u32 len)
{
	unsigned long flags;

	if (!len || len > TEBOX_MALI_EVT_DATA)
		return;
	spin_lock_irqsave(&tf->ev_lock, flags);
	if (tf->ev_n < TBX_EV_Q) {
		u8 i = (tf->ev_head + tf->ev_n) % TBX_EV_Q;

		memcpy(tf->ev_data[i], data, len);
		tf->ev_len[i] = len;
		tf->ev_n++;
	}
	spin_unlock_irqrestore(&tf->ev_lock, flags);
}

static int tbx_evt_pop(struct tebox_mali_file *tf, void *dst, u32 cap)
{
	unsigned long flags;
	int n = 0;

	spin_lock_irqsave(&tf->ev_lock, flags);
	if (tf->ev_n) {
		n = min_t(u32, tf->ev_len[tf->ev_head], cap);
		memcpy(dst, tf->ev_data[tf->ev_head], n);
		tf->ev_head = (tf->ev_head + 1) % TBX_EV_Q;
		tf->ev_n--;
	}
	spin_unlock_irqrestore(&tf->ev_lock, flags);
	return n;
}

static void tbx_evt_readable(struct tebox_mali_dev *td, u32 ctx,
			     const u8 *data, u32 len)
{
	struct tebox_mali_file *tf;
	unsigned long flags;

	if (ctx >= TBX_MAX_CTX)
		return;
	spin_lock_irqsave(&td->ctx_lock, flags);
	tf = td->ctx_files[ctx];
	if (tf) {
		if (len)
			tbx_evt_push(tf, data, len);
		WRITE_ONCE(tf->readable, true);
		wake_up_interruptible(&tf->wq);
	}
	spin_unlock_irqrestore(&td->ctx_lock, flags);
}

static void tbx_evt_post(struct tebox_mali_dev *td, struct tebox_mali_event *ev)
{
	struct scatterlist sg;

	sg_init_one(&sg, ev, sizeof(*ev));
	virtqueue_add_inbuf(td->evt_vq, &sg, 1, ev, GFP_ATOMIC);
}

static void tebox_mali_evt_done(struct virtqueue *vq)
{
	struct tebox_mali_dev *td = vq->vdev->priv;
	struct tebox_mali_event *ev;
	unsigned int len;
	unsigned long flags;
	bool need;

	while ((ev = virtqueue_get_buf(vq, &len))) {
		if (len >= sizeof(*ev)) {
			if (ev->type == TEBOX_MALI_EVT_READABLE)
				tbx_evt_readable(td, ev->ctx, ev->data,
						 ev->data_len);
			else if (ev->type == TEBOX_MALI_EVT_FENCE)
				tbx_fence_host_signaled(td, (u32)ev->id);
		}
		tbx_evt_post(td, ev);
	}
	/* Recycle inbufs; coalesce the notify like the CMD path. */
	spin_lock_irqsave(&td->vq_lock, flags);
	need = virtqueue_kick_prepare(vq);
	spin_unlock_irqrestore(&td->vq_lock, flags);
	if (need)
		virtqueue_notify(vq);
}

/*
 * One synchronous request. The request buffer is [cmd_hdr][op payload][extra
 * out]; the response buffer is [cmd_resp][resp payload]. |op_in|/|op_in_len| is
 * the fixed op struct; |xtra|/|xtra_len| is trailing out-data (e.g. the ioctl
 * struct). |op_out|/|op_out_cap| receives the response payload.
 */
static int tebox_mali_cmd(struct tebox_mali_dev *td, u32 op, u32 ctx,
			  const void *op_in, u32 op_in_len,
			  const void *xtra, u32 xtra_len,
			  void *op_out, u32 op_out_cap, u32 *op_out_len)
{
	struct tbx_cmd_slot *s;
	struct tebox_mali_cmd_hdr *hdr;
	struct tebox_mali_cmd_resp *resp;
	struct scatterlist sg_out, sg_in, *sgs[2];
	u32 req_len, want;
	unsigned long flags;
	int ret;

	if (sizeof(*hdr) + op_in_len + xtra_len > TBX_CMD_BUF ||
	    sizeof(*resp) + op_out_cap > TBX_CMD_BUF)
		return -EMSGSIZE;

	s = tbx_slot_get(td);
	if (!s)
		return -ERESTARTSYS;

	if (op == TEBOX_MALI_OP_IOCTL)
		tbx_c_ioctl++;
	else if (op == TEBOX_MALI_OP_MMAP)
		tbx_c_mmap++;
	else if (op == TEBOX_MALI_OP_MUNMAP)
		tbx_c_munmap++;

	hdr = s->req;
	hdr->op = op;
	hdr->ctx = ctx;
	hdr->seq = ++td->seq;
	hdr->len = op_in_len + xtra_len;
	if (op_in_len)
		memcpy((u8 *)s->req + sizeof(*hdr), op_in, op_in_len);
	if (xtra_len)
		memcpy((u8 *)s->req + sizeof(*hdr) + op_in_len, xtra, xtra_len);
	req_len = sizeof(*hdr) + op_in_len + xtra_len;

	resp = s->resp;
	memset(resp, 0, sizeof(*resp));

	sg_init_one(&sg_out, s->req, req_len);
	sg_init_one(&sg_in, s->resp, sizeof(*resp) + op_out_cap);
	sgs[0] = &sg_out;
	sgs[1] = &sg_in;

	reinit_completion(&s->done);
	spin_lock_irqsave(&td->vq_lock, flags);
	ret = virtqueue_add_sgs(td->cmd_vq, sgs, 1, 1, s, GFP_KERNEL);
	if (!ret) {
		/* Coalesce kicks across concurrent slots — each notify is a
		 * KVM exit on the host. */
		if (virtqueue_kick_prepare(td->cmd_vq)) {
			spin_unlock_irqrestore(&td->vq_lock, flags);
			virtqueue_notify(td->cmd_vq);
		} else {
			spin_unlock_irqrestore(&td->vq_lock, flags);
		}
	} else {
		spin_unlock_irqrestore(&td->vq_lock, flags);
	}
	if (ret)
		goto out;

	if (!wait_for_completion_timeout(&s->done, msecs_to_jiffies(5000))) {
		ret = -ETIMEDOUT;
		goto out;
	}

	ret = resp->result;
	if (ret >= 0 && op_out && resp->len) {
		want = min_t(u32, resp->len, op_out_cap);
		memcpy(op_out, (u8 *)s->resp + sizeof(*resp), want);
		if (op_out_len)
			*op_out_len = want;
	} else if (op_out_len) {
		*op_out_len = 0;
	}
out:
	tbx_slot_release(td, s);
	return ret;
}

/* Kick the host for any deferred CS_EVENT_SIGNAL fires. Called when the batch
 * fills, when a sync op lands, or when fire_timer expires. */
static void tbx_fire_kick(struct tebox_mali_dev *td)
{
	bool kick = false;
	unsigned long flags;

	spin_lock_irqsave(&td->vq_lock, flags);
	if (td->fire_pending) {
		td->fire_pending = 0;
		kick = virtqueue_kick_prepare(td->cmd_vq);
	}
	spin_unlock_irqrestore(&td->vq_lock, flags);
	if (kick)
		virtqueue_notify(td->cmd_vq);
}

static enum hrtimer_restart tbx_fire_timer_fn(struct hrtimer *t)
{
	struct tebox_mali_dev *td =
		container_of(t, struct tebox_mali_dev, fire_timer);

	tbx_fire_kick(td);
	return HRTIMER_NORESTART;
}

static void tbx_mmap_kick(struct tebox_mali_dev *td)
{
	bool kick = false;
	unsigned long flags;

	spin_lock_irqsave(&td->vq_lock, flags);
	if (td->mmap_pending) {
		td->mmap_pending = 0;
		kick = virtqueue_kick_prepare(td->cmd_vq);
	}
	spin_unlock_irqrestore(&td->vq_lock, flags);
	if (kick)
		virtqueue_notify(td->cmd_vq);
}

static enum hrtimer_restart tbx_mmap_timer_fn(struct hrtimer *t)
{
	struct tebox_mali_dev *td =
		container_of(t, struct tebox_mali_dev, mmap_timer);

	tbx_mmap_kick(td);
	return HRTIMER_NORESTART;
}

static void tbx_cs_poll_arm(struct tebox_mali_dev *td);
static enum hrtimer_restart tbx_cs_poll_timer_fn(struct hrtimer *t);
static void tbx_cs_poll_work_fn(struct work_struct *work);

/* Same as tebox_mali_cmd, but do not wait: used for CS_EVENT_SIGNAL and
 * no-output KCPU enqueues. The host still writes a response; cmd_done frees
 * the slot. Falls back to a sync send if every slot is busy. */
static int tebox_mali_cmd_fire(struct tebox_mali_dev *td, u32 op, u32 ctx,
			       const void *op_in, u32 op_in_len,
			       const void *xtra, u32 xtra_len)
{
	struct tbx_cmd_slot *s;
	struct tebox_mali_cmd_hdr *hdr;
	struct scatterlist sg_out, sg_in, *sgs[2];
	u32 req_len;
	unsigned long flags;
	int i, ret;

	if (sizeof(*hdr) + op_in_len + xtra_len > TBX_CMD_BUF)
		return -EMSGSIZE;

	s = NULL;
	spin_lock_irqsave(&td->slot_lock, flags);
	for (i = 0; i < TBX_CMD_SLOTS; i++) {
		if (!td->slots[i].used) {
			s = &td->slots[i];
			s->used = true;
			s->fire = true;
			break;
		}
	}
	spin_unlock_irqrestore(&td->slot_lock, flags);
	if (!s)
		return tebox_mali_cmd(td, op, ctx, op_in, op_in_len, xtra,
				      xtra_len, NULL, 0, NULL);

	if (op == TEBOX_MALI_OP_IOCTL)
		tbx_c_ioctl++;
	else if (op == TEBOX_MALI_OP_MMAP)
		tbx_c_mmap++;
	else if (op == TEBOX_MALI_OP_MUNMAP)
		tbx_c_munmap++;

	hdr = s->req;
	hdr->op = op;
	hdr->ctx = ctx;
	hdr->seq = ++td->seq;
	hdr->len = op_in_len + xtra_len;
	if (op_in_len)
		memcpy((u8 *)s->req + sizeof(*hdr), op_in, op_in_len);
	if (xtra_len)
		memcpy((u8 *)s->req + sizeof(*hdr) + op_in_len, xtra, xtra_len);
	req_len = sizeof(*hdr) + op_in_len + xtra_len;
	memset(s->resp, 0, sizeof(struct tebox_mali_cmd_resp));

	sg_init_one(&sg_out, s->req, req_len);
	sg_init_one(&sg_in, s->resp, sizeof(struct tebox_mali_cmd_resp));
	sgs[0] = &sg_out;
	sgs[1] = &sg_in;

	spin_lock_irqsave(&td->vq_lock, flags);
	ret = virtqueue_add_sgs(td->cmd_vq, sgs, 1, 1, s, GFP_KERNEL);
	if (!ret) {
		if (virtqueue_kick_prepare(td->cmd_vq)) {
			spin_unlock_irqrestore(&td->vq_lock, flags);
			virtqueue_notify(td->cmd_vq);
		} else {
			spin_unlock_irqrestore(&td->vq_lock, flags);
		}
	} else {
		spin_unlock_irqrestore(&td->vq_lock, flags);
	}
	if (ret) {
		tbx_slot_release(td, s);
		return ret;
	}
	return 0;
}

static void tbx_cs_poll_arm(struct tebox_mali_dev *td)
{
	if (tbx_cs_poll_us <= 0)
		return;
	/* hrtimer: usecs_to_jiffies(500) rounds to ~10ms on HZ=100. */
	hrtimer_start(&td->cs_poll_timer,
		      ns_to_ktime((u64)tbx_cs_poll_us * 1000ull),
		      HRTIMER_MODE_REL);
}

static enum hrtimer_restart tbx_cs_poll_timer_fn(struct hrtimer *t)
{
	struct tebox_mali_dev *td =
		container_of(t, struct tebox_mali_dev, cs_poll_timer);

	if (tbx_cs_poll_us <= 0 || !atomic_read(&td->open_files))
		return HRTIMER_NORESTART;
	schedule_work(&td->cs_poll_work);
	hrtimer_forward_now(t, ns_to_ktime((u64)tbx_cs_poll_us * 1000ull));
	return HRTIMER_RESTART;
}

static void tbx_cs_poll_work_fn(struct work_struct *work)
{
	struct tebox_mali_dev *td =
		container_of(work, struct tebox_mali_dev, cs_poll_work);
	struct tebox_mali_ioctl_req sreq = {
		.cmd = _IO(KBASE_IOCTL_TYPE, KBASE_NR_CS_EVENT_SIGNAL),
	};
	unsigned long flags;
	u32 ctxs[TBX_MAX_CTX];
	int n = 0, i;

	if (tbx_cs_poll_us <= 0 || !atomic_read(&td->open_files))
		return;
	spin_lock_irqsave(&td->ctx_lock, flags);
	for (i = 0; i < TBX_MAX_CTX; i++) {
		struct tebox_mali_file *tf = td->ctx_files[i];

		if (tf && tf->open_ok)
			ctxs[n++] = tf->ctx;
	}
	spin_unlock_irqrestore(&td->ctx_lock, flags);
	for (i = 0; i < n; i++)
		(void)tebox_mali_cmd_fire(td, TEBOX_MALI_OP_IOCTL, ctxs[i],
					  &sreq, sizeof(sreq), NULL, 0);
	tbx_fire_kick(td);
}

/* ---- buffer mapping cache ------------------------------------------------ */

/* Flush coalesced munmaps. Caller holds tf->map_lock. */
static void tbx_munmap_flush(struct tebox_mali_file *tf)
{
	struct tebox_mali_munmap_batch_req br;
	u32 n = tf->munmap_n;
	int ret;

	if (!n)
		return;
	tf->munmap_n = 0;
	if (n == 1) {
		struct tebox_mali_munmap_req req = {
			.handle = tf->munmap_pending[0],
		};

		tebox_mali_cmd(tf->td, TEBOX_MALI_OP_MUNMAP, tf->ctx,
			       &req, sizeof(req), NULL, 0, NULL, 0, NULL);
		return;
	}
	br.n = n;
	br.reserved = 0;
	ret = tebox_mali_cmd(tf->td, TEBOX_MALI_OP_MUNMAP_BATCH, tf->ctx,
			     &br, sizeof(br), tf->munmap_pending,
			     n * sizeof(u32), NULL, 0, NULL);
	if (ret) {
		/* Host too old / error: fall back one-by-one. */
		u32 i;

		for (i = 0; i < n; i++) {
			struct tebox_mali_munmap_req req = {
				.handle = tf->munmap_pending[i],
			};

			tebox_mali_cmd(tf->td, TEBOX_MALI_OP_MUNMAP, tf->ctx,
				       &req, sizeof(req), NULL, 0, NULL, 0,
				       NULL);
		}
	}
}

/* Find a mapping for |cookie| that hasn't been consumed by .mmap yet. kbase
 * recycles cookies once a buffer is mmap'd, so only a still-pending entry may be
 * reused to correlate this syscall's get_unmapped_area with its .mmap. */
static struct tebox_mali_map_ent *
tbx_map_lookup(struct tebox_mali_file *tf, u64 cookie)
{
	int i;

	for (i = 0; i < TBX_MAP_CACHE; i++)
		if (tf->maps[i].used && !tf->maps[i].mapped &&
		    tf->maps[i].cookie == cookie)
			return &tf->maps[i];
	return NULL;
}

/* Install a host mmap_resp into the per-open cache (no virtio). Caller holds
 * map_lock. Used for eager mmap piggybacked on MEM_ALLOC. */
static struct tebox_mali_map_ent *
tbx_map_install(struct tebox_mali_file *tf, u64 cookie,
		const struct tebox_mali_mmap_resp *r)
{
	struct tebox_mali_map_ent *e = tbx_map_lookup(tf, cookie);
	int i;

	if (e)
		return e;
	for (i = 0; i < TBX_MAP_CACHE; i++)
		if (!tf->maps[i].used)
			break;
	if (i == TBX_MAP_CACHE) {
		tbx_dbg("mmap-cache-full ctx=%u cookie=0x%llx\n", tf->ctx, cookie);
		return NULL;
	}
	e = &tf->maps[i];
	memset(e, 0, sizeof(*e));
	e->used = true;
	e->cookie = cookie;
	e->gpu_va = r->gpu_va;
	e->hm_off = r->hostmem_offset;
	e->size = r->size;
	e->handle = r->handle;
	e->host_prot = (r->flags & TEBOX_MALI_MMAP_F_PROT) ? r->host_prot :
			(PROT_READ | PROT_WRITE);
	tbx_dbg("mmap-eager ctx=%u cookie=0x%llx gpu_va=0x%llx size=0x%llx "
		"hm_off=0x%llx handle=%u prot=0x%x\n",
		tf->ctx, cookie, e->gpu_va, e->size, e->hm_off, e->handle,
		e->host_prot);
	return e;
}

/* Ask the host to map the buffer at |cookie| (the mmap pgoff) of |len| bytes.
 * The host mmaps its kbase fd FIRST so kbase pins the GPU VA to the host
 * address, and returns that address; we cache it so the user VMA can be placed
 * there. Idempotent per cookie. Caller holds tf->map_lock.
 *
 * Also used as a MEM_ALLOC prefetch: after alloc returns a SAME_VA cookie we
 * OP_MMAP immediately so get_unmapped_area is a cache hit (one less sync
 * virtio on the mmap syscall hot path). */
static struct tebox_mali_map_ent *
tbx_map_create(struct tebox_mali_file *tf, u64 cookie, u64 len)
{
	struct tebox_mali_mmap_req req = { .pgoff = cookie, .size = len };
	struct tebox_mali_mmap_resp r = {};
	struct tebox_mali_map_ent *e = tbx_map_lookup(tf, cookie);
	int i, ret;

	if (e)
		return e;
	/* Drain pending frees before carving a new hostmem slice. */
	tbx_munmap_flush(tf);
	ret = tebox_mali_cmd(tf->td, TEBOX_MALI_OP_MMAP, tf->ctx,
			     &req, sizeof(req), NULL, 0, &r, sizeof(r), NULL);
	if (ret) {
		tbx_dbg("mmap-host ctx=%u cookie=0x%llx len=0x%llx -> %d pid=%d\n",
			tf->ctx, cookie, len, ret, task_pid_nr(current));
		return NULL;
	}
	for (i = 0; i < TBX_MAP_CACHE; i++)
		if (!tf->maps[i].used)
			break;
	if (i == TBX_MAP_CACHE) {
		tbx_dbg("mmap-cache-full ctx=%u cookie=0x%llx\n", tf->ctx, cookie);
		return NULL;
	}
	e = &tf->maps[i];
	memset(e, 0, sizeof(*e));
	e->used = true;
	e->cookie = cookie;
	e->gpu_va = r.gpu_va;
	e->hm_off = r.hostmem_offset;
	e->size = r.size;
	e->handle = r.handle;
	e->host_prot = (r.flags & TEBOX_MALI_MMAP_F_PROT) ? r.host_prot :
			(PROT_READ | PROT_WRITE);
	tbx_dbg("mmap-host ctx=%u cookie=0x%llx gpu_va=0x%llx size=0x%llx "
		"hm_off=0x%llx handle=%u prot=0x%x pid=%d\n",
		tf->ctx, cookie, e->gpu_va, e->size, e->hm_off, e->handle,
		e->host_prot, task_pid_nr(current));
	return e;
}

/* Drop the host mapping (kbase sees the munmap native libmali would issue).
 * Only after every guest VMA/PTE over its BAR pages is gone. Caller holds
 * tf->map_lock. Handles are coalesced into OP_MUNMAP_BATCH. */
static void tbx_map_release(struct tebox_mali_file *tf,
			    struct tebox_mali_map_ent *e)
{
	u32 handle = e->handle;

	tbx_dbg("munmap ctx=%u cookie=0x%llx gpu_va=0x%llx handle=%u pid=%d\n",
		tf->ctx, e->cookie, e->gpu_va, handle, task_pid_nr(current));
	memset(e, 0, sizeof(*e));
	if (!handle)
		return;
	tf->munmap_pending[tf->munmap_n++] = handle;
	/* Flush early enough that free storms collapse, but don't wait for 64
	 * if the burst is shorter — 8 is enough to cut virtio by ~8×. */
	if (tf->munmap_n >= 8)
		tbx_munmap_flush(tf);
}

/* ---- file ops ------------------------------------------------------------ */

static int tebox_mali_open(struct inode *ino, struct file *file)
{
	struct tebox_mali_dev *td =
		container_of(file->private_data, struct tebox_mali_dev, misc);
	struct tebox_mali_file *tf;
	struct tebox_mali_open_resp r = {};
	int ret;

	tf = kzalloc(sizeof(*tf), GFP_KERNEL);
	if (!tf)
		return -ENOMEM;
	tf->td = td;
	mutex_init(&tf->map_lock);
	spin_lock_init(&tf->ev_lock);
	init_waitqueue_head(&tf->wq);

	ret = tebox_mali_cmd(td, TEBOX_MALI_OP_OPEN, 0, NULL, 0, NULL, 0,
			     &r, sizeof(r), NULL);
	if (ret) {
		kfree(tf);
		return ret;
	}
	if (r.ctx >= TBX_MAX_CTX) {
		tebox_mali_cmd(td, TEBOX_MALI_OP_CLOSE, r.ctx,
			       NULL, 0, NULL, 0, NULL, 0, NULL);
		kfree(tf);
		return -ENOSPC;
	}
	tf->ctx = r.ctx;
	tf->open_ok = true;
	spin_lock_irq(&td->ctx_lock);
	td->ctx_files[tf->ctx] = tf;
	spin_unlock_irq(&td->ctx_lock);
	file->private_data = tf;
	if (atomic_inc_return(&td->open_files) == 1)
		tbx_cs_poll_arm(td);
	tbx_dbg("open ctx=%u pid=%d (%s)\n", tf->ctx, task_pid_nr(current),
		current->comm);
	return 0;
}

static int tebox_mali_release(struct inode *ino, struct file *file)
{
	struct tebox_mali_file *tf = file->private_data;

	tbx_dbg("close ctx=%u pid=%d (%s)\n", tf->ctx, task_pid_nr(current),
		current->comm);
	spin_lock_irq(&tf->td->ctx_lock);
	if (tf->td->ctx_files[tf->ctx] == tf)
		tf->td->ctx_files[tf->ctx] = NULL;
	spin_unlock_irq(&tf->td->ctx_lock);
	mutex_lock(&tf->map_lock);
	tbx_munmap_flush(tf);
	mutex_unlock(&tf->map_lock);
	if (tf->open_ok)
		tebox_mali_cmd(tf->td, TEBOX_MALI_OP_CLOSE, tf->ctx,
			       NULL, 0, NULL, 0, NULL, 0, NULL);
	kfree(tf);
	return 0;
}

/* Describe the embedded user pointers of the ioctls that carry them, so the
 * host can relocate them to its own buffers. |sbuf| is the (copied-in) ioctl
 * struct; sizes of the pointed-to data are computed from its fields. */
static int tbx_ioctl_indirects(unsigned int cmd, const void *sbuf, u32 size,
			       struct tebox_mali_ioctl_ind *inds)
{
	if (_IOC_TYPE(cmd) != KBASE_IOCTL_TYPE)
		return 0;

	if (_IOC_NR(cmd) == KBASE_NR_GET_GPUPROPS && size >= 16) {
		u64 buffer;
		u32 psize;

		memcpy(&buffer, sbuf, 8);
		memcpy(&psize, (const u8 *)sbuf + 8, 4);
		if (!buffer || !psize)
			return 0;	/* size-query call: no pointer data */
		inds[0] = (struct tebox_mali_ioctl_ind){
			.field_off = 0, .len = psize, .dir = TEBOX_MALI_IND_OUT };
		return 1;
	}
	if (_IOC_NR(cmd) == KBASE_NR_CS_GET_GLB_IFACE && size >= 24) {
		u32 mgn, mtsn;
		u64 gp, sp;
		int n = 0;

		memcpy(&mgn, sbuf, 4);
		memcpy(&mtsn, (const u8 *)sbuf + 4, 4);
		memcpy(&gp, (const u8 *)sbuf + 8, 8);
		memcpy(&sp, (const u8 *)sbuf + 16, 8);
		if (gp && mgn)		/* basep_cs_group_control = 16 bytes */
			inds[n++] = (struct tebox_mali_ioctl_ind){
				.field_off = 8, .len = mgn * 16,
				.dir = TEBOX_MALI_IND_OUT };
		if (sp && mtsn)		/* basep_cs_stream_control = 8 bytes */
			inds[n++] = (struct tebox_mali_ioctl_ind){
				.field_off = 16, .len = mtsn * 8,
				.dir = TEBOX_MALI_IND_OUT };
		return n;
	}
	/* STICKY_RESOURCE_{MAP,UNMAP}: {u64 count; u64 address} — address
	 * points at count GPU VAs (u64 each). SF/RenderEngine uses this
	 * after MEM_IMPORT for external textures. */
	if ((_IOC_NR(cmd) == KBASE_NR_STICKY_RESOURCE_MAP ||
	     _IOC_NR(cmd) == KBASE_NR_STICKY_RESOURCE_UNMAP) && size >= 16) {
		u64 count, address;

		memcpy(&count, sbuf, 8);
		memcpy(&address, (const u8 *)sbuf + 8, 8);
		if (!address || !count || count > (1u << 16))
			return 0;
		inds[0] = (struct tebox_mali_ioctl_ind){
			.field_off = 8, .len = (u32)(count * 8),
			.dir = TEBOX_MALI_IND_IN };
		return 1;
	}
	return 0;
}

/* MEM_IMPORT (UMM/dmabuf): the host can't see the guest dmabuf fd, so resolve
 * it to its guest-physical page segments and let the host import them into
 * kbase as a USER_BUFFER (the GPU then DMAs the guest pages in place). Returns
 * -EOPNOTSUPP if the import isn't a dmabuf so the caller can fall back. */
static long tbx_mem_import_umm(struct tebox_mali_file *tf, unsigned long arg)
{
	u8 s[24];				/* union kbase_ioctl_mem_import */
	u64 flags, phandle;
	u32 type;
	int fd, ret, nseg, i, j;
	struct dma_buf *dmabuf;
	struct dma_buf_attachment *attach;
	struct sg_table *sgt;
	struct scatterlist *sg;
	struct tebox_mali_import_req ireq;
	struct tebox_mali_import_resp iresp = {};
	struct tebox_mali_import_seg *segs;

	if (copy_from_user(s, (void __user *)arg, sizeof(s)))
		return -EFAULT;
	memcpy(&flags, s, 8);
	memcpy(&phandle, s + 8, 8);
	memcpy(&type, s + 16, 4);
	if (type != BASE_MEM_IMPORT_TYPE_UMM)
		return -EOPNOTSUPP;		/* caller falls back to generic */
	if (get_user(fd, (int __user *)(uintptr_t)phandle))
		return -EFAULT;

	dmabuf = dma_buf_get(fd);
	if (IS_ERR(dmabuf))
		return PTR_ERR(dmabuf);
	pr_info("tbx-import: fd=%d dmabuf=%px size=%zu\n", fd, dmabuf,
		dmabuf->size);
	attach = dma_buf_attach(dmabuf, &tf->td->vdev->dev);
	if (IS_ERR(attach)) {
		pr_info("tbx-import: attach err %ld\n", PTR_ERR(attach));
		ret = PTR_ERR(attach);
		goto put;
	}
	/* We only need the backing pages (sg_phys), not a device DMA mapping. The
	 * virtio device's DMA map of a system-heap buffer is degenerate and its
	 * unmap cache-sync faults, so skip CPU sync on map/unmap. */
	attach->dma_map_attrs = DMA_ATTR_SKIP_CPU_SYNC;
	pr_info("tbx-import: attached\n");
	sgt = dma_buf_map_attachment(attach, DMA_BIDIRECTIONAL);
	if (IS_ERR(sgt)) {
		pr_info("tbx-import: map err %ld\n", PTR_ERR(sgt));
		ret = PTR_ERR(sgt);
		goto detach;
	}
	nseg = sgt->orig_nents;
	pr_info("tbx-import: mapped nseg=%d nents=%d\n", nseg, sgt->nents);
	if (nseg < 1 || nseg > TBX_MAX_SEG) {
		ret = -E2BIG;
		goto unmap;
	}
	segs = kcalloc(nseg, sizeof(*segs), GFP_KERNEL);
	if (!segs) {
		ret = -ENOMEM;
		goto unmap;
	}
	i = 0;
	for_each_sgtable_sg(sgt, sg, j) {	/* j: macro's own iterator */
		segs[i].gpa = sg_phys(sg);	/* guest physical page addr */
		segs[i].len = sg->length;
		if (++i >= nseg)
			break;
	}
	pr_info("tbx-import: %d segs, seg0 gpa=0x%llx len=%llu\n", i,
		(u64)segs[0].gpa, (u64)segs[0].len);
	ireq.flags = flags;
	ireq.type = type;
	ireq.n_seg = i;
	ret = tebox_mali_cmd(tf->td, TEBOX_MALI_OP_IMPORT, tf->ctx,
			     &ireq, sizeof(ireq), segs, i * sizeof(*segs),
			     &iresp, sizeof(iresp), NULL);
	kfree(segs);
	pr_info("tbx-import: cmd ret=%d gpu_va=0x%llx\n", ret, iresp.gpu_va);
	if (ret)
		goto unmap;

	/* out struct: {u64 flags; u64 gpu_va; u64 va_pages} overlays the union. */
	memcpy(s + 8, &iresp.gpu_va, 8);
	memcpy(s + 16, &iresp.va_pages, 8);
	if (copy_to_user((void __user *)arg, s, sizeof(s)))
		ret = -EFAULT;
unmap:
	pr_info("tbx-import: unmap\n");
	dma_buf_unmap_attachment(attach, sgt, DMA_BIDIRECTIONAL);
detach:
	pr_info("tbx-import: detach\n");
	dma_buf_detach(dmabuf, attach);
put:
	pr_info("tbx-import: put, ret=%d\n", ret);
	dma_buf_put(dmabuf);
	return ret;
}

/* Indirect buffers of one forwarded ioctl: bufs[i] holds IN data before the
 * send and receives OUT data after it; uptr[i] is the user pointer. */
struct tbx_ind_set {
	int n;
	struct tebox_mali_ioctl_ind inds[TBX_MAX_IND];
	void *bufs[TBX_MAX_IND];
	u64 uptr[TBX_MAX_IND];
};

/* Append a descriptor whose IN data (if any) is copied from user |uptr|. */
static int tbx_ind_add(struct tbx_ind_set *s, u32 parent, u32 field_off,
		       u32 len, u32 dir, u64 uptr)
{
	int i = s->n;

	if (i >= TBX_MAX_IND || len > TBX_CMD_BUF)
		return -EMSGSIZE;
	s->bufs[i] = kvzalloc(len ? len : 1, GFP_KERNEL);
	if (!s->bufs[i])
		return -ENOMEM;
	s->n++;
	s->inds[i] = (struct tebox_mali_ioctl_ind){
		.field_off = field_off, .len = len, .dir = dir, .parent = parent };
	s->uptr[i] = uptr;
	if ((dir & TEBOX_MALI_IND_IN) && uptr && len &&
	    copy_from_user(s->bufs[i], (void __user *)(uintptr_t)uptr, len))
		return -EFAULT;
	return 0;
}

static void tbx_ind_free(struct tbx_ind_set *s, int from)
{
	int i;

	for (i = from; i < s->n; i++)
		kvfree(s->bufs[i]);
	s->n = from;
}

/* Send |kstruct| plus indirect buffers; on success |kstruct| holds the
 * struct as the host kernel left it and OUT buffers are filled.
 * For MEM_ALLOC(_EX), the host may append an eager mmap_resp; install it
 * into the map cache under |alloc_va_pages| (0 = not an alloc). */
static int tbx_ioctl_send(struct tebox_mali_file *tf, unsigned int cmd,
			  u8 *kstruct, u32 size, struct tbx_ind_set *s,
			  u64 alloc_va_pages)
{
	struct tebox_mali_ioctl_req req = { .cmd = cmd, .size = size,
					    .n_ind = s->n };
	u32 in_data = 0, out_total = 0, xtra_len, resp_cap, got = 0;
	u8 *xtra, *resp;
	size_t o;
	int i, ret;
	bool want_eager = alloc_va_pages != 0;

	for (i = 0; i < s->n; i++) {
		if (s->inds[i].dir & TEBOX_MALI_IND_IN)
			in_data += s->inds[i].len;
		if (s->inds[i].dir & TEBOX_MALI_IND_OUT)
			out_total += s->inds[i].len;
	}
	/* out-data to host: [struct(size)][n_ind*ind][IN buffers...] */
	xtra_len = size + s->n * sizeof(s->inds[0]) + in_data;
	resp_cap = size + out_total;
	if (want_eager)
		resp_cap += sizeof(struct tebox_mali_mmap_resp);
	if (xtra_len > TBX_CMD_BUF ||
	    sizeof(struct tebox_mali_cmd_resp) + resp_cap > TBX_CMD_BUF)
		return -EMSGSIZE;
	xtra = kvzalloc(xtra_len ? xtra_len : 1, GFP_KERNEL);
	resp = kvzalloc(resp_cap ? resp_cap : 1, GFP_KERNEL);
	if (!xtra || !resp) {
		ret = -ENOMEM;
		goto out;
	}
	o = 0;
	memcpy(xtra + o, kstruct, size);
	o += size;
	memcpy(xtra + o, s->inds, s->n * sizeof(s->inds[0]));
	o += s->n * sizeof(s->inds[0]);
	for (i = 0; i < s->n; i++) {
		if (s->inds[i].dir & TEBOX_MALI_IND_IN) {
			memcpy(xtra + o, s->bufs[i], s->inds[i].len);
			o += s->inds[i].len;
		}
	}

	if (!out_total && _IOC_TYPE(cmd) == KBASE_IOCTL_TYPE &&
	    _IOC_NR(cmd) == KBASE_NR_CS_EVENT_SIGNAL) {
		ret = tebox_mali_cmd_fire(tf->td, TEBOX_MALI_OP_IOCTL, tf->ctx,
					  &req, sizeof(req), xtra, xtra_len);
		goto out;
	}
	ret = tebox_mali_cmd(tf->td, TEBOX_MALI_OP_IOCTL, tf->ctx,
			     &req, sizeof(req), xtra, xtra_len,
			     resp, resp_cap, &got);
	if (ret < 0)
		goto out;
	/* resp: [struct(size)][OUT buffers...][optional eager mmap_resp] */
	memcpy(kstruct, resp, size);
	o = size;
	for (i = 0; i < s->n; i++) {
		if (s->inds[i].dir & TEBOX_MALI_IND_OUT) {
			memcpy(s->bufs[i], resp + o, s->inds[i].len);
			o += s->inds[i].len;
		}
	}
	if (want_eager && got >= o + sizeof(struct tebox_mali_mmap_resp)) {
		struct tebox_mali_mmap_resp mr;
		u64 gpu_va, cookie;

		memcpy(&mr, resp + o, sizeof(mr));
		memcpy(&gpu_va, kstruct + 8, 8); /* out.gpu_va after alloc */
		cookie = gpu_va >> 12;
		if (cookie >= TBX_COOKIE_FIRST && cookie <= TBX_COOKIE_LAST &&
		    mr.size >= (alloc_va_pages << 12)) {
			mutex_lock(&tf->map_lock);
			tbx_map_install(tf, cookie, &mr);
			mutex_unlock(&tf->map_lock);
		}
	}
out:
	kvfree(xtra);
	kvfree(resp);
	return ret;
}

/* Nested array behind a KCPU command's info pointer: bytes, or 0 if none. */
static u32 tbx_kcpu_nested_len(const u8 *c, u32 *dir)
{
	u32 nr;
	u8 count;

	memcpy(&nr, c + 16, 4);
	count = c[16];
	*dir = TEBOX_MALI_IND_IN;
	switch (c[0]) {
	case TBX_KCPU_FENCE_SIGNAL:
		*dir = TEBOX_MALI_IND_IN | TEBOX_MALI_IND_OUT |
		       TEBOX_MALI_IND_FENCE_OUT;
		return TBX_BASE_FENCE_SIZE;
	case TBX_KCPU_CQS_WAIT:			/* base_cqs_wait_info */
		return nr * 16;
	case TBX_KCPU_CQS_SET:			/* base_cqs_set */
		return nr * 8;
	case TBX_KCPU_CQS_WAIT_OPERATION:	/* base_cqs_wait_operation_info */
	case TBX_KCPU_CQS_SET_OPERATION:	/* base_cqs_set_operation_info */
		return nr * 24;
	case TBX_KCPU_JIT_ALLOC:
		return count * TBX_JIT_ALLOC_INFO_SIZE;
	case TBX_KCPU_JIT_FREE:			/* u8 ids[] */
		return count;
	default:	/* imports carry GPU VAs; no user memory */
		return 0;
	}
}

/* Submit cmds[0..n) as one host enqueue, then turn each FENCE_SIGNAL's host
 * fence id into a guest sync_file fd written back to the user's base_fence. */
static int tbx_kcpu_submit(struct tebox_mali_file *tf, unsigned int cmd,
			   const u8 *kenq, u8 *cmds, u32 n,
			   struct tbx_ind_set *s)
{
	u8 req[16];
	int ret, k;

	memcpy(req, kenq, 16);
	memcpy(req + 8, &n, 4);			/* nr_commands */
	memcpy(s->bufs[0], cmds, n * TBX_KCPU_CMD_SIZE);
	s->inds[0].len = n * TBX_KCPU_CMD_SIZE;
	{
		int has_out = 0;

		for (k = 0; k < s->n; k++) {
			if (s->inds[k].dir & (TEBOX_MALI_IND_OUT |
					      TEBOX_MALI_IND_FENCE_OUT)) {
				has_out = 1;
				break;
			}
		}
		if (!has_out) {
			struct tebox_mali_ioctl_req ireq = {
				.cmd = cmd, .size = 16, .n_ind = s->n };
			u32 in_data = 0, xtra_len, o;
			u8 *xtra;

			for (k = 0; k < s->n; k++)
				if (s->inds[k].dir & TEBOX_MALI_IND_IN)
					in_data += s->inds[k].len;
			xtra_len = 16 + s->n * sizeof(s->inds[0]) + in_data;
			if (xtra_len > TBX_CMD_BUF)
				return -EMSGSIZE;
			xtra = kvzalloc(xtra_len, GFP_KERNEL);
			if (!xtra)
				return -ENOMEM;
			o = 0;
			memcpy(xtra + o, req, 16);
			o += 16;
			memcpy(xtra + o, s->inds, s->n * sizeof(s->inds[0]));
			o += s->n * sizeof(s->inds[0]);
			for (k = 0; k < s->n; k++) {
				if (s->inds[k].dir & TEBOX_MALI_IND_IN) {
					memcpy(xtra + o, s->bufs[k],
					       s->inds[k].len);
					o += s->inds[k].len;
				}
			}
			ret = tebox_mali_cmd_fire(tf->td, TEBOX_MALI_OP_IOCTL,
						  tf->ctx, &ireq, sizeof(ireq),
						  xtra, xtra_len);
			kvfree(xtra);
			return ret;
		}
	}
	ret = tbx_ioctl_send(tf, cmd, req, 16, s, 0);
	if (ret < 0)
		return ret;
	for (k = 1; k < s->n; k++) {
		s32 id;
		int fd;

		if (!(s->inds[k].dir & TEBOX_MALI_IND_FENCE_OUT))
			continue;
		memcpy(&id, s->bufs[k], 4);
		if (id <= 0)
			continue;
		fd = tbx_fence_install(tf->td, id);
		if (fd < 0)
			return fd;
		if (put_user(fd, (int __user *)(uintptr_t)s->uptr[k]))
			return -EFAULT;
	}
	return 0;
}

/* KCPU_QUEUE_ENQUEUE: {u64 addr; u32 nr_commands; u8 id; u8 pad[3]}. The
 * command array and each command's own array are user memory, and fences are
 * fds, so relocate both levels and translate fences. FENCE_WAIT splits the
 * submission: the commands after it are only sent once it signaled. */
static long tbx_kcpu_enqueue(struct tebox_mali_file *tf, unsigned int cmd,
			     unsigned long arg)
{
	struct tbx_ind_set *s;
	u8 kenq[16], *cmds;
	u64 addr;
	u32 nr, i, start, bytes;
	long ret;

	if (copy_from_user(kenq, (void __user *)arg, sizeof(kenq)))
		return -EFAULT;
	memcpy(&addr, kenq, 8);
	memcpy(&nr, kenq + 8, 4);
	if (!nr || nr > TBX_KCPU_MAX_CMDS)
		return nr ? -EINVAL : 0;
	cmds = kvmalloc_array(nr, TBX_KCPU_CMD_SIZE, GFP_KERNEL);
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!cmds || !s) {
		ret = -ENOMEM;
		goto out;
	}
	if (copy_from_user(cmds, (void __user *)(uintptr_t)addr,
			   (size_t)nr * TBX_KCPU_CMD_SIZE)) {
		ret = -EFAULT;
		goto out;
	}
	ret = tbx_ind_add(s, 0, 0, TBX_KCPU_MAX_CMDS * TBX_KCPU_CMD_SIZE,
			  TEBOX_MALI_IND_IN, 0);
	if (ret)
		goto out;

	start = 0;
	bytes = 0;
	for (i = 0; i <= nr; i++) {
		u8 *c = cmds + (size_t)i * TBX_KCPU_CMD_SIZE;
		u32 len = 0, dir = 0;
		u64 ptr = 0;

		if (i < nr && c[0] != TBX_KCPU_FENCE_WAIT) {
			len = tbx_kcpu_nested_len(c, &dir);
			memcpy(&ptr, c + 8, 8);
			if (!ptr)
				len = 0;
		}
		/* Flush the pending run before a FENCE_WAIT, at the end, or when
		 * this command wouldn't fit in one request. */
		if (i > start &&
		    (i == nr || c[0] == TBX_KCPU_FENCE_WAIT ||
		     (len && s->n >= TBX_MAX_IND) ||
		     (i - start + 1) * TBX_KCPU_CMD_SIZE + bytes + len +
		     (s->n + 1) * sizeof(struct tebox_mali_ioctl_ind) + 64 >
		     TBX_CMD_BUF)) {
			ret = tbx_kcpu_submit(tf, cmd, kenq,
					      cmds + (size_t)start *
					      TBX_KCPU_CMD_SIZE,
					      i - start, s);
			tbx_ind_free(s, 1);
			if (ret)
				goto out;
			start = i;
			bytes = 0;
		}
		if (i == nr)
			break;
		if (c[0] == TBX_KCPU_FENCE_WAIT) {
			ret = tbx_fence_wait_user(tf->td, c);
			if (ret)
				goto out;
			start = i + 1;
			continue;
		}
		if (len) {
			ret = tbx_ind_add(s, 1,
					  (i - start) * TBX_KCPU_CMD_SIZE + 8,
					  len, dir, ptr);
			if (ret)
				goto out;
			bytes += len;
		}
	}
	ret = 0;
out:
	if (s)
		tbx_ind_free(s, 0);
	kfree(s);
	kvfree(cmds);
	/* Unreal/Vulkan CSF rarely QUEUE_KICKs; work is submitted via USER_IO
	 * doorbells + KCPU. Follow every successful enqueue with an event
	 * signal so host kbase wakes the MCU (same as the QUEUE_KICK path). */
	if (!ret) {
		struct tebox_mali_ioctl_req sreq = {
			.cmd = _IO(KBASE_IOCTL_TYPE, KBASE_NR_CS_EVENT_SIGNAL),
		};
		(void)tebox_mali_cmd_fire(tf->td, TEBOX_MALI_OP_IOCTL, tf->ctx,
					  &sreq, sizeof(sreq), NULL, 0);
		tbx_fire_kick(tf->td);
	}
	return ret;
}

/* STREAM_CREATE must look like a sw_sync timeline fd: libmali issues
 * SW_SYNC_IOC_CREATE_FENCE / INC on it. Guest GKI has no CONFIG_SW_SYNC, so
 * emulate a timeline that hands out already-signaled sync_files. */
#define TBX_SW_SYNC_IOC_MAGIC		'W'
#define TBX_SW_SYNC_IOC_CREATE_FENCE	_IOWR(TBX_SW_SYNC_IOC_MAGIC, 0, \
					      struct tbx_sw_sync_create_fence)
#define TBX_SW_SYNC_IOC_INC		_IOW(TBX_SW_SYNC_IOC_MAGIC, 1, __u32)

struct tbx_sw_sync_create_fence {
	__u32 value;
	char name[32];
	__s32 fence;
};

static const char *tbx_stream_timeline_name(struct dma_fence *f)
{
	return "stream";
}

static const struct dma_fence_ops tbx_stream_fence_ops = {
	.get_driver_name = tbx_fence_driver_name,
	.get_timeline_name = tbx_stream_timeline_name,
};

static int tbx_signaled_sync_fd(struct tebox_mali_dev *td)
{
	static atomic64_t stream_seq;
	struct tbx_fence *f;
	struct sync_file *sf;
	int fd;

	f = kzalloc(sizeof(*f), GFP_KERNEL);
	if (!f)
		return -ENOMEM;
	dma_fence_init(&f->base, &tbx_stream_fence_ops, &td->fence_lock,
		       td->fence_ctx, (u64)atomic64_inc_return(&stream_seq));
	dma_fence_signal(&f->base);
	sf = sync_file_create(&f->base);
	dma_fence_put(&f->base);
	if (!sf)
		return -ENOMEM;
	fd = get_unused_fd_flags(O_CLOEXEC);
	if (fd < 0) {
		fput(sf->file);
		return fd;
	}
	fd_install(fd, sf->file);
	return fd;
}

static long tbx_stream_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct tebox_mali_dev *td = file->private_data;
	struct tbx_sw_sync_create_fence data;
	int fd;

	if (cmd == TBX_SW_SYNC_IOC_INC)
		return 0;
	if (cmd != TBX_SW_SYNC_IOC_CREATE_FENCE)
		return -ENOTTY;
	if (copy_from_user(&data, (void __user *)arg, sizeof(data)))
		return -EFAULT;
	fd = tbx_signaled_sync_fd(td);
	if (fd < 0)
		return fd;
	data.fence = fd;
	if (copy_to_user((void __user *)arg, &data, sizeof(data))) {
		/* Best-effort: userspace still owns fd on fault. */
		return -EFAULT;
	}
	return 0;
}

static const struct file_operations tbx_stream_fops = {
	.owner = THIS_MODULE,
	.llseek = noop_llseek,
	.unlocked_ioctl = tbx_stream_ioctl,
	.compat_ioctl = tbx_stream_ioctl,
};

static void tbx_log_ioctl(struct tebox_mali_file *tf, unsigned int cmd,
			  const void *kstruct, u32 size, long ret)
{
	unsigned int nr = _IOC_NR(cmd);
	const char *name;
	u64 gva = 0;

	if (!tbx_debug)
		return;
	name = tbx_nr_name(nr);
	/* MEM_FREE: {u64 gpu_addr}. MEM_ALLOC(_EX): gpu_va is the last u64. */
	if (kstruct && size >= 8 && nr == KBASE_NR_MEM_FREE)
		memcpy(&gva, kstruct, 8);
	else if (kstruct && size >= 40 && ret >= 0 &&
		 (nr == KBASE_NR_MEM_ALLOC || nr == KBASE_NR_MEM_ALLOC_EX))
		memcpy(&gva, (const u8 *)kstruct + 32, 8);
	if (name && gva)
		pr_info("tebox-mali: ioctl %s ctx=%u gpu_va=0x%llx -> %ld pid=%d (%s)\n",
			name, tf->ctx, gva, ret, task_pid_nr(current),
			current->comm);
	else if (name)
		pr_info("tebox-mali: ioctl %s ctx=%u size=%u -> %ld pid=%d (%s)\n",
			name, tf->ctx, size, ret, task_pid_nr(current),
			current->comm);
	else
		pr_info("tebox-mali: ioctl nr=%u ctx=%u cmd=0x%x size=%u -> %ld "
			"pid=%d (%s)\n",
			nr, tf->ctx, cmd, size, ret, task_pid_nr(current),
			current->comm);
}

static long tebox_mali_ioctl(struct file *file, unsigned int cmd,
			     unsigned long arg)
{
	struct tebox_mali_file *tf = file->private_data;
	u32 size = _IOC_SIZE(cmd);
	struct tebox_mali_ioctl_ind inds[TBX_MAX_FLAT_IND];
	struct tbx_ind_set *s = NULL;
	u8 *kstruct = NULL;
	int n_ind, ret, i;
	u64 uptr;

	if (_IOC_TYPE(cmd) == KBASE_IOCTL_TYPE) {
		/* MEM_IMPORT of a dmabuf needs guest-physical page resolution. */
		if (_IOC_NR(cmd) == KBASE_NR_MEM_IMPORT && size >= 24) {
			long r = tbx_mem_import_umm(tf, arg);

			if (r != -EOPNOTSUPP) {
				tbx_log_ioctl(tf, cmd, NULL, size, r);
				return r;	/* else: non-dmabuf import */
			}
		}
		if (_IOC_NR(cmd) == KBASE_NR_KCPU_QUEUE_ENQUEUE && size == 16) {
			ret = tbx_kcpu_enqueue(tf, cmd, arg);
			tbx_log_ioctl(tf, cmd, NULL, size, ret);
			return ret;
		}
		if (_IOC_NR(cmd) == KBASE_NR_FENCE_VALIDATE && size == 4) {
			ret = tbx_fence_validate(arg);
			tbx_log_ioctl(tf, cmd, NULL, size, ret);
			return ret;
		}
	}

	if (size > TBX_CMD_BUF)
		return -EMSGSIZE;
	kstruct = kzalloc(size ? size : 1, GFP_KERNEL);
	s = kzalloc(sizeof(*s), GFP_KERNEL);
	if (!kstruct || !s) {
		ret = -ENOMEM;
		goto out;
	}
	if (size && (_IOC_DIR(cmd) & _IOC_WRITE) &&
	    copy_from_user(kstruct, (void __user *)arg, size)) {
		ret = -EFAULT;
		goto out;
	}

	n_ind = tbx_ioctl_indirects(cmd, kstruct, size, inds);
	for (i = 0; i < n_ind; i++) {
		memcpy(&uptr, kstruct + inds[i].field_off, 8);
		ret = tbx_ind_add(s, 0, inds[i].field_off, inds[i].len,
				  inds[i].dir, uptr);
		if (ret)
			goto out;
	}

	/* MEM_ALLOC overlays out on in; capture va_pages before the host write.
	 * Host may eager-mmap and append mmap_resp (see tbx_ioctl_send). */
	{
		unsigned int nr = _IOC_NR(cmd);
		u64 alloc_va_pages = 0;

		if (_IOC_TYPE(cmd) == KBASE_IOCTL_TYPE && size >= 8 &&
		    (nr == KBASE_NR_MEM_ALLOC || nr == KBASE_NR_MEM_ALLOC_EX))
			memcpy(&alloc_va_pages, kstruct, 8);

		ret = tbx_ioctl_send(tf, cmd, kstruct, size, s, alloc_va_pages);
		if (ret < 0)
			goto out;
	}

	/* Copy the struct back for _IOWR and scatter each OUT buffer. */
	if (size && (_IOC_DIR(cmd) & _IOC_READ) &&
	    copy_to_user((void __user *)arg, kstruct, size)) {
		ret = -EFAULT;
		goto out;
	}
	for (i = 0; i < s->n; i++) {
		if ((s->inds[i].dir & TEBOX_MALI_IND_OUT) && s->uptr[i] &&
		    copy_to_user((void __user *)(uintptr_t)s->uptr[i],
				 s->bufs[i], s->inds[i].len)) {
			ret = -EFAULT;
			goto out;
		}
	}
	/* Host timeline fd is closed on the host and meaningless here. Always
	 * mint a guest fake sw_sync timeline (CREATE_FENCE → signaled
	 * sync_file) — even when the host STREAM_CREATE fails (e.g. composer
	 * opens mali0 without SET_FLAGS and host returns EPERM). Soft HWC
	 * PresentFence depends on this path. */
	if (_IOC_TYPE(cmd) == KBASE_IOCTL_TYPE &&
	    _IOC_NR(cmd) == KBASE_NR_STREAM_CREATE) {
		int sfd = anon_inode_getfd("[tebox-mali-stream]", &tbx_stream_fops,
					   tf->td, O_RDONLY | O_CLOEXEC);
		if (sfd >= 0)
			ret = sfd;
	}
out:
	tbx_log_ioctl(tf, cmd, kstruct, size, ret);
	if (s)
		tbx_ind_free(s, 0);
	kfree(s);
	kfree(kstruct);
	return ret;
}

/* Refcount VMAs over one host mapping: mprotect/munmap of part of a buffer
 * splits the VMA (open() on the new piece), and only the last close may drop
 * the host mapping — by then the PTEs are zapped and the TLB flushed, so no
 * vCPU can still reach the BAR pages. */
static void tebox_mali_vm_open(struct vm_area_struct *vma)
{
	struct tebox_mali_file *tf = vma->vm_file->private_data;
	struct tebox_mali_map_ent *e = vma->vm_private_data;

	mutex_lock(&tf->map_lock);
	e->users++;
	mutex_unlock(&tf->map_lock);
}

static void tebox_mali_vm_close(struct vm_area_struct *vma)
{
	struct tebox_mali_file *tf = vma->vm_file->private_data;
	struct tebox_mali_map_ent *e = vma->vm_private_data;

	mutex_lock(&tf->map_lock);
	if (e->users && --e->users == 0) {
		/* Process death unmaps in address order; leave the host side to
		 * OP_CLOSE, which unbinds the queues before freeing buffers
		 * (native kbase defers these frees to context teardown). */
		if (!atomic_read(&vma->vm_mm->mm_users))
			e->handle = 0;
		tbx_map_release(tf, e);
	}
	/* If this was the last close in a burst, push the batch soon — but
	 * leave coalescing to tbx_map_release's threshold; only force-flush
	 * when the mm is going away so we don't leak handles across close. */
	if (!atomic_read(&vma->vm_mm->mm_users))
		tbx_munmap_flush(tf);
	mutex_unlock(&tf->map_lock);
}

/* Moving a SAME_VA mapping would break CPU VA == GPU VA. */
static int tebox_mali_vm_mremap(struct vm_area_struct *vma)
{
	return -EINVAL;
}

static const struct vm_operations_struct tebox_mali_vm_ops = {
	.open = tebox_mali_vm_open,
	.close = tebox_mali_vm_close,
	.mremap = tebox_mali_vm_mremap,
};

static int tebox_mali_mmap(struct file *file, struct vm_area_struct *vma)
{
	struct tebox_mali_file *tf = file->private_data;
	u64 size = vma->vm_end - vma->vm_start;
	struct tebox_mali_map_ent *e;
	int ret = 0;

	/* get_unmapped_area already asked the host to map (and placed the VMA at
	 * the GPU VA). Fall back to creating it here for MAP_FIXED callers. */
	mutex_lock(&tf->map_lock);
	e = tbx_map_lookup(tf, vma->vm_pgoff);
	if (!e)
		e = tbx_map_create(tf, vma->vm_pgoff, size);
	if (!e) {
		ret = -ENOMEM;
		goto out;
	}
	if (e->size < size) {
		ret = -EINVAL;
		goto out;
	}
	/* Same rule as native kbase ("inconsistent VM flags"): never map wider
	 * than the region's CPU access. A writable PTE over read-only host pages
	 * kills the VM. The entry stays pending so a retry reuses its GPU VA. */
	if (((vma->vm_flags & VM_READ) && !(e->host_prot & PROT_READ)) ||
	    ((vma->vm_flags & VM_WRITE) && !(e->host_prot & PROT_WRITE))) {
		ret = -EPERM;
		goto out;
	}

	vm_flags_set(vma, VM_DONTEXPAND | VM_DONTDUMP | VM_DONTCOPY);
	if (!(e->host_prot & PROT_WRITE))
		vm_flags_clear(vma, VM_MAYWRITE);
	if (!(e->host_prot & PROT_READ))
		vm_flags_clear(vma, VM_MAYREAD | VM_MAYEXEC);

	/* Map the user VMA straight onto the buffer's pages in the hostmem BAR
	 * (shared with the host kbase buffer the GPU DMAs) -> zero copy.
	 * GPU-only (PROT_NONE) buffers have no BAR pages: the VMA just reserves
	 * the GPU VA in this process. */
	if (e->host_prot & (PROT_READ | PROT_WRITE)) {
		unsigned long pfn = (tf->td->shm_base + e->hm_off) >> PAGE_SHIFT;
		pgprot_t prot = vma->vm_page_prot;

		if (vma->vm_pgoff >= TBX_USER_IO_FIRST &&
		    vma->vm_pgoff <= TBX_USER_IO_LAST)
			prot = pgprot_writecombine(prot);

		if (remap_pfn_range(vma, vma->vm_start, pfn, size, prot)) {
			ret = -EAGAIN;
			goto out;
		}
	}
	e->mapped = true;	/* consume: kbase may now recycle this cookie */
	e->users = 1;
	vma->vm_private_data = e;
	vma->vm_ops = &tebox_mali_vm_ops;
out:
	tbx_dbg("mmap ctx=%u pgoff=0x%lx va=0x%lx size=0x%llx -> %d pid=%d\n",
		tf->ctx, vma->vm_pgoff, vma->vm_start, size, ret,
		task_pid_nr(current));
	mutex_unlock(&tf->map_lock);
	return ret;
}

/* SAME_VA across the VM boundary: the host maps the buffer first and tells us
 * the GPU VA (= host map address); we place the user mapping at that exact
 * address so the guest CPU pointer equals the GPU VA, as kbase requires. */
static unsigned long tebox_mali_get_unmapped_area(struct file *file,
						  unsigned long addr,
						  unsigned long len,
						  unsigned long pgoff,
						  unsigned long flags)
{
	struct tebox_mali_file *tf = file->private_data;
	struct tebox_mali_map_ent *e;
	unsigned long va;

	/* Not SAME_VA: the host VA is meaningless to the guest and lands in the
	 * host's top-down mmap area, which overlaps Android's library area. Let
	 * this process pick its own free range; .mmap creates the host side. */
	if (pgoff < TBX_COOKIE_FIRST || pgoff > TBX_COOKIE_LAST)
		return current->mm->get_unmapped_area(file, addr, len, pgoff,
						      flags);

	mutex_lock(&tf->map_lock);
	e = tbx_map_create(tf, pgoff, len);
	if (!e) {
		mutex_unlock(&tf->map_lock);
		return -ENOMEM;
	}
	va = (unsigned long)e->gpu_va;
	/* Host free-mmap lands in ~0x5f… (CIX SAME_VA zone). Cookie-sized
	 * "VAs" mean the host failed to realize the buffer. */
	if (va < 0x100000000UL || va + len < va || va + len > TASK_SIZE) {
		pr_warn_ratelimited("tebox-mali: GPU VA 0x%lx+0x%lx unusable in pid %d\n",
				    va, len, task_pid_nr(current));
		tbx_map_release(tf, e);
		mutex_unlock(&tf->map_lock);
		return -ENOMEM;
	}
	/*
	 * If something already occupies the host-chosen SAME_VA, clear it.
	 * Returning -ENOMEM here left mali userspace with the alloc cookie as
	 * a CPU pointer (SIGSEGV write@0x43000 in Unreal). mmap_lock is held.
	 */
	if (!(flags & MAP_FIXED) &&
	    find_vma_intersection(current->mm, va, va + len)) {
		pr_warn_ratelimited("tebox-mali: clearing 0x%lx+0x%lx for SAME_VA pid %d\n",
				    va, len, task_pid_nr(current));
		if (tbx_do_munmap)
			tbx_do_munmap(current->mm, va, len, NULL);
		else {
			tbx_map_release(tf, e);
			mutex_unlock(&tf->map_lock);
			return -ENOMEM;
		}
	}
	mutex_unlock(&tf->map_lock);
	return va;
}

/* The stock Mali userspace learns of GPU completion by poll()+read()ing the
 * kbase fd (base_csf_notification records). Forward it to the host fd. The
 * host read is non-blocking (-EAGAIN when no event); blocking callers sleep
 * until the host's EVT_READABLE, which it re-arms after every OP_READ. */
static ssize_t tebox_mali_read(struct file *file, char __user *ubuf, size_t len,
			       loff_t *ppos)
{
	struct tebox_mali_file *tf = file->private_data;
	struct tebox_mali_read_req req;
	void *kbuf;
	u32 cap, got = 0;
	int ret;

	cap = min_t(size_t, len, TBX_CMD_BUF - sizeof(struct tebox_mali_cmd_resp));
	if (!cap)
		return 0;
	kbuf = kmalloc(cap, GFP_KERNEL);
	if (!kbuf)
		return -ENOMEM;
	ret = tbx_evt_pop(tf, kbuf, cap);
	if (ret > 0) {
		if (copy_to_user(ubuf, kbuf, ret))
			ret = -EFAULT;
		kfree(kbuf);
		return ret;
	}
	req.max_len = cap;
	req.reserved = 0;
	for (;;) {
		WRITE_ONCE(tf->readable, false);
		ret = tbx_evt_pop(tf, kbuf, cap);
		if (ret > 0) {
			if (copy_to_user(ubuf, kbuf, ret))
				ret = -EFAULT;
			break;
		}
		ret = tebox_mali_cmd(tf->td, TEBOX_MALI_OP_READ, tf->ctx,
				     &req, sizeof(req), NULL, 0, kbuf, cap, &got);
		if (ret >= 0) {
			if (got && copy_to_user(ubuf, kbuf, got))
				ret = -EFAULT;
			else
				ret = got;
			break;
		}
		if (ret == -EAGAIN && !(file->f_flags & O_NONBLOCK)) {
			if (wait_event_interruptible(tf->wq,
						     READ_ONCE(tf->readable))) {
				ret = -ERESTARTSYS;
				break;
			}
			continue;
		}
		break;
	}
	kfree(kbuf);
	return ret;
}

static __poll_t tebox_mali_poll(struct file *file, poll_table *wait)
{
	struct tebox_mali_file *tf = file->private_data;

	poll_wait(file, &tf->wq, wait);
	if (READ_ONCE(tf->ev_n) || READ_ONCE(tf->readable))
		return EPOLLIN | EPOLLRDNORM;
	return 0;
}

static const struct file_operations tebox_mali_fops = {
	.owner = THIS_MODULE,
	.open = tebox_mali_open,
	.release = tebox_mali_release,
	.unlocked_ioctl = tebox_mali_ioctl,
	.compat_ioctl = tebox_mali_ioctl,
	.mmap = tebox_mali_mmap,
	.get_unmapped_area = tebox_mali_get_unmapped_area,
	.read = tebox_mali_read,
	.poll = tebox_mali_poll,
	.llseek = noop_llseek,
};

/* ---- virtio probe -------------------------------------------------------- */

static int tebox_mali_probe(struct virtio_device *vdev)
{
	struct tebox_mali_dev *td;
	struct virtio_shm_region shm;
	vq_callback_t *cbs[TEBOX_MALI_VQ_MAX] = { tebox_mali_cmd_done,
						  tebox_mali_evt_done };
	static const char * const names[TEBOX_MALI_VQ_MAX] = { "cmd", "evt" };
	struct virtqueue *vqs[TEBOX_MALI_VQ_MAX];
	int ret, i;

	ret = tbx_resolve_do_munmap();
	if (ret)
		dev_warn(&vdev->dev,
			 "tebox-mali: do_munmap lookup failed (%d); SAME_VA collision clear disabled\n",
			 ret);

	td = devm_kzalloc(&vdev->dev, sizeof(*td), GFP_KERNEL);
	if (!td)
		return -ENOMEM;
	td->vdev = vdev;
	vdev->priv = td;
	spin_lock_init(&td->vq_lock);
	spin_lock_init(&td->slot_lock);
	init_waitqueue_head(&td->slot_wq);
	spin_lock_init(&td->ctx_lock);
	spin_lock_init(&td->fence_lock);
	xa_init_flags(&td->fences, XA_FLAGS_LOCK_IRQ);
	td->fence_ctx = dma_fence_context_alloc(1);
	hrtimer_init(&td->fire_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	td->fire_timer.function = tbx_fire_timer_fn;
	hrtimer_init(&td->mmap_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	td->mmap_timer.function = tbx_mmap_timer_fn;
	hrtimer_init(&td->cs_poll_timer, CLOCK_MONOTONIC, HRTIMER_MODE_REL);
	td->cs_poll_timer.function = tbx_cs_poll_timer_fn;
	INIT_WORK(&td->cs_poll_work, tbx_cs_poll_work_fn);
	atomic_set(&td->open_files, 0);

	for (i = 0; i < TBX_CMD_SLOTS; i++) {
		td->slots[i].req = devm_kzalloc(&vdev->dev, TBX_CMD_BUF,
						GFP_KERNEL);
		td->slots[i].resp = devm_kzalloc(&vdev->dev, TBX_CMD_BUF,
						 GFP_KERNEL);
		init_completion(&td->slots[i].done);
		if (!td->slots[i].req || !td->slots[i].resp)
			return -ENOMEM;
	}

	ret = virtio_find_vqs(vdev, TEBOX_MALI_VQ_MAX, vqs, cbs, names, NULL);
	if (ret)
		return ret;
	td->cmd_vq = vqs[TEBOX_MALI_VQ_CMD];
	td->evt_vq = vqs[TEBOX_MALI_VQ_EVT];
	for (i = 0; i < TBX_EVT_BUFS; i++)
		tbx_evt_post(td, &td->evt_bufs[i]);

	if (!virtio_get_shm_region(vdev, &shm, TEBOX_MALI_SHM_ID_HOSTMEM)) {
		dev_err(&vdev->dev, "no hostmem shared-memory region\n");
		ret = -ENODEV;
		goto del_vqs;
	}
	td->shm_base = shm.addr;
	td->shm_len = shm.len;

	virtio_device_ready(vdev);
	virtqueue_kick(td->evt_vq);

	td->misc.minor = MISC_DYNAMIC_MINOR;
	td->misc.name = "mali0";
	td->misc.fops = &tebox_mali_fops;
	td->misc.mode = 0666;
	ret = misc_register(&td->misc);
	if (ret)
		goto del_vqs;

	dev_info(&vdev->dev,
		 "tebox-mali ready: /dev/mali0 -> host kbase, hostmem %llu MiB @ 0x%llx\n",
		 (u64)td->shm_len >> 20, (u64)td->shm_base);
	return 0;

del_vqs:
	vdev->config->del_vqs(vdev);
	return ret;
}

static void tebox_mali_remove(struct virtio_device *vdev)
{
	struct tebox_mali_dev *td = vdev->priv;

	hrtimer_cancel(&td->fire_timer);
	hrtimer_cancel(&td->mmap_timer);
	hrtimer_cancel(&td->cs_poll_timer);
	cancel_work_sync(&td->cs_poll_work);
	misc_deregister(&td->misc);
	vdev->config->reset(vdev);
	vdev->config->del_vqs(vdev);
}

static const struct virtio_device_id tebox_mali_id_table[] = {
	{ TEBOX_MALI_VIRTIO_ID, VIRTIO_DEV_ANY_ID },
	{ 0 },
};
MODULE_DEVICE_TABLE(virtio, tebox_mali_id_table);

static struct virtio_driver tebox_mali_driver = {
	.driver.name = "tebox_mali",
	.driver.owner = THIS_MODULE,
	.id_table = tebox_mali_id_table,
	.probe = tebox_mali_probe,
	.remove = tebox_mali_remove,
};
module_virtio_driver(tebox_mali_driver);

MODULE_DESCRIPTION("tebox-mali guest shim: CIX Mali kbase /dev/mali0 passthrough");
MODULE_IMPORT_NS(DMA_BUF);
MODULE_LICENSE("GPL");
