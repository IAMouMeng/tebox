/*
 * tebox-mali (local, NOT for upstream): a QEMU virtio device that passes the
 * host CIX Mali kbase char device /dev/mali0 (Mali-G720, CSF, DDK r49p0) through
 * to a QEMU guest, so the guest's Mali userspace (stock CIX blob or open PanVK)
 * drives the real GPU directly instead of via gfxstream/virgl translation.
 *
 * See tebox-mali-proto.h and thirdparty/vendor-drivers/cix/mali/.
 *
 * Control plane: generic kbase ioctl forward over the CMD vq (kbase's CSF
 * ioctls are flat, pointer-free structs). One guest open() == one host fd.
 *
 * Memory plane: kbase is SAME_VA and its buffers / CS user-IO pages are plain
 * mmap()s of the fd. On a guest mmap the host mmap()s its fd at the same cookie,
 * wraps the pages as a RAM MemoryRegion inside the device's hostmem PCI BAR, and
 * returns the window offset; the guest shim remaps the user VMA onto that BAR
 * page. KVM maps guest-PA -> the very host pages the GPU DMAs, so CPU and GPU
 * share one buffer with no copy.
 *
 * Mapping lifecycle mirrors native libmali exactly: the host mmap()s with the
 * region's own CPU access (PROT_NONE for GPU-only memory, which then needs no
 * BAR pages), and the guest's last VMA close sends OP_MUNMAP, so BAR window
 * space and map slots are reclaimed instead of leaking until -ENOMEM storms.
 */
#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/aio.h"
#include "hw/virtio/virtio.h"
#include "hw/virtio/virtio-pci.h"
#include "hw/pci/pci.h"
#include "hw/core/qdev-properties.h"
#include "system/memory.h"
#include "system/address-spaces.h"
#include "system/ramblock.h"
#include "exec/memattrs.h"
#include "qapi/error.h"
#include "qom/object.h"

#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "tebox-mali-proto.h"

#define TYPE_TEBOX_MALI "tebox-mali"
OBJECT_DECLARE_SIMPLE_TYPE(TeboxMali, TEBOX_MALI)

#define TEBOX_MALI_NODE      "/dev/mali0"
#define TEBOX_MALI_MAX_CTX   64        /* concurrent guest opens (processes)   */
/* Live host mappings across all contexts (reclaimed by OP_MUNMAP). */
#define TEBOX_MALI_MAX_MAPS  4096
#define TEBOX_MALI_PAGE      4096      /* host and guest are both 4K           */
/* hostmem window: guest PA address space only; real memory is consumed solely
 * by the sub-regions actually mapped. Default 2 GiB. */
/* libmali keeps hundreds of MiB CPU-mapped per GL process (launcher and
 * systemui ~500 MiB each); 2 GiB ran out within minutes of boot. */
#define TEBOX_MALI_HOSTMEM_DEFAULT (16ull << 30)
#define TEBOX_MALI_IOCTL_MAX  4096     /* largest flat kbase ioctl struct      */
#define TEBOX_MALI_MAX_IND    64       /* max embedded pointers per ioctl      */
#define TEBOX_MALI_MAX_FENCES 1024     /* unsignaled KCPU FENCE_SIGNAL fds     */
#define TEBOX_MALI_MAX_IMPORTS 1024    /* live dmabuf imports, all contexts    */

/* kbase UAPI bits needed to learn a SAME_VA allocation's CPU access. A 64-bit
 * SAME_VA MEM_ALLOC returns out.gpu_va = a cookie in [COOKIE_BASE, +64 pages),
 * which libmali then mmap()s with exactly the region's CPU_RD/CPU_WR. */
#define TBXM_KBASE_IOCTL_TYPE        0x80
#define TBXM_KBASE_NR_MEM_ALLOC      5     /* union kbase_ioctl_mem_alloc (32)  */
#define TBXM_KBASE_NR_MEM_ALLOC_EX   59    /* union kbase_ioctl_mem_alloc_ex (64) */
#define TBXM_KBASE_NR_MEM_FREE       7     /* struct kbase_ioctl_mem_free (8)   */
#define TBXM_KBASE_NR_STREAM_CREATE  24    /* returns a sync timeline fd        */
#define TBXM_KBASE_NR_QUEUE_REGISTER 36    /* struct kbase_ioctl_cs_queue_register */
#define TBXM_KBASE_NR_QUEUE_REGISTER_EX 40 /* register_ex: same buffer_gpu_addr */
#define TBXM_KBASE_NR_QUEUE_BIND     39    /* union kbase_ioctl_cs_queue_bind   */
#define TBXM_KBASE_NR_QUEUE_TERMINATE 41   /* terminate by buffer_gpu_addr (ring) */
#define TBXM_KBASE_NR_GROUP_TERMINATE 43   /* struct kbase_ioctl_cs_queue_group_term */
#define TBXM_KBASE_NR_HEAP_INIT      48    /* out.gpu_heap_va at offset 0       */
#define TBXM_KBASE_NR_HEAP_TERM      49    /* struct kbase_ioctl_cs_tiler_heap_term */
#define TBXM_MAX_HEAPS               16
#define TBXM_MAX_QUEUES              128   /* live CSF queues tracked per ctx  */
#define TBXM_BASE_MEM_PROT_CPU_RD    (1ull << 0)
#define TBXM_BASE_MEM_PROT_CPU_WR    (1ull << 1)
#define TBXM_USER_REG_PG             0x2f  /* BASEP_MEM_CSF_USER_REG_PAGE_HANDLE */
#define TBXM_USER_IO_FIRST           0x30  /* BASEP_MEM_CSF_USER_IO_PAGES_HANDLE */
#define TBXM_COOKIE_FIRST            0x40  /* BASE_MEM_COOKIE_BASE >> PAGE_SHIFT */
#define TBXM_NR_COOKIES              64
#define TBXM_COOKIE_KNOWN            0x80  /* cookie_prot[] entry is valid      */

/*
 * Hot-path logging: SF/bootanim fires thousands of MEM_ALLOC_EX / MEM_FREE /
 * CS_EVENT ioctls per second. Unconditional info_report on each one pegs the
 * QEMU process (serial + docker logs) at ~100% CPU. Opt in with TEBOX_MALI_TRACE=1.
 */
static bool tbxm_trace(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("TEBOX_MALI_TRACE");
        cached = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

/*
 * Heap BAR mapping object. MemoryRegion lifetime is RCU-bound: after
 * del_subregion + object_unparent the GPA must stay reserved in hm_used
 * until finalize/reclaim, or KVM_SET_USER_MEMORY_REGION returns EEXIST.
 */
#define TYPE_TEBOX_MALI_BUF "tebox-mali-buf"
struct TeboxMaliBuf {
    Object parent_obj;
    MemoryRegion mr;
    struct TeboxMali *m;
    uint64_t hm_off;
    uint64_t size;
    void *host_ptr;
};
OBJECT_DECLARE_SIMPLE_TYPE(TeboxMaliBuf, TEBOX_MALI_BUF)

typedef struct TeboxMaliReclaim {
    struct TeboxMali *m;
    uint64_t hm_off;
    uint64_t size;
    void *host_ptr;
} TeboxMaliReclaim;

/* One mapped kbase buffer / CS user-IO page block exposed into the BAR. */
typedef struct TeboxMaliMap {
    bool used;
    int ctx;
    void *host_ptr;         /* host mmap(); also kept on buf until reclaim  */
    uint64_t pgoff;         /* the kbase mmap cookie (gpu_va or handle)     */
    uint64_t size;
    uint64_t hm_off;        /* byte offset within the hostmem window        */
    int host_prot;          /* PROT_* of the host mmap                      */
    TeboxMaliBuf *buf;      /* non-NULL => live BAR MemoryRegion            */
} TeboxMaliMap;

typedef struct TeboxMaliCtx {
    bool used;
    bool watching;          /* fd registered for EVT_READABLE               */
    QEMUTimer *rearm;       /* re-watch if the guest never reads after EVT  */
    int fd;                 /* host /dev/mali0 fd for this guest open       */
    uint32_t id;
    struct TeboxMali *owner;
    /* PROT_* | TBXM_COOKIE_KNOWN per pending SAME_VA cookie, from MEM_ALLOC. */
    uint8_t cookie_prot[TBXM_NR_COOKIES];
    /* Live tiler heaps (gpu_heap_va, 0 = free), terminated at CLOSE. */
    uint64_t heap_va[TBXM_MAX_HEAPS];
    /*
     * CSF queues: QUEUE_TERMINATE / NO_USER_FREE clear keys off the ring
     * buffer GPU VA (base_addr), NOT the USER_IO mmap. Passing USER_IO to
     * terminate is a no-op; then MEM_FREE(ring) hits NO_USER_FREE, and a
     * confused retry has previously corrupted kbase's VA rbtree (host panic
     * in kbase_add_va_region_rbtree under AnTuTu).
     */
    struct {
        uint64_t ring_va;       /* buffer_gpu_addr from REGISTER; 0 = free   */
        uint64_t user_io_cookie;/* BIND out.mmap_handle (pgoff), 0 = unbound */
    } queue[TBXM_MAX_QUEUES];
} TeboxMaliCtx;

/* A guest dmabuf imported as a host USER_BUFFER. kbase only frees a SAME_VA
 * region when its last mapping goes, so the import lives until the guest's
 * MEM_FREE of |gva| or ctx close. */
typedef struct TeboxMaliImport {
    bool used;
    int ctx;
    void *gva;              /* host mmap of the import cookie (== GPU VA)   */
    size_t gva_size;
    void *buf;              /* contiguous host VA of the guest pages        */
    uint64_t buf_size;
    bool buf_is_mmap;       /* munmap(buf) vs address_space_unmap           */
} TeboxMaliImport;

/* A host sync_file from KCPU FENCE_SIGNAL, watched until it signals. */
typedef struct TeboxMaliFence {
    int fd;                 /* -1: free slot */
    uint32_t id;
    struct TeboxMali *owner;
} TeboxMaliFence;

struct TeboxMali {
    VirtIODevice parent_obj;
    VirtQueue *cmd_vq;
    VirtQueue *evt_vq;
    GArray *evt_pending;    /* struct tebox_mali_event awaiting a guest buffer */
    MemoryRegion hostmem;   /* the PCI BAR window (registered by the proxy) */
    uint64_t hostmem_size;
    unsigned long *hm_used; /* one bit per window page                      */
    uint32_t next_fence_id;
    TeboxMaliCtx ctx[TEBOX_MALI_MAX_CTX];
    TeboxMaliMap map[TEBOX_MALI_MAX_MAPS];
    TeboxMaliFence fence[TEBOX_MALI_MAX_FENCES];
    TeboxMaliImport import[TEBOX_MALI_MAX_IMPORTS];
};

/* ---- helpers ------------------------------------------------------------- */

static TeboxMaliCtx *tbxm_ctx(TeboxMali *m, uint32_t id)
{
    if (id >= TEBOX_MALI_MAX_CTX || !m->ctx[id].used) {
        return NULL;
    }
    return &m->ctx[id];
}

static int tbxm_ioctl(int fd, unsigned long req, void *arg)
{
    int r;
    do {
        r = ioctl(fd, req, arg);
    } while (r < 0 && errno == EINTR);
    return r;
}

static void tbxm_unmap_slot(TeboxMali *m, TeboxMaliMap *mp);
static void G_GNUC_PRINTF(1, 2) tbxm_warn_rl(const char *fmt, ...);

static void tbxm_reclaim_bh(void *opaque)
{
    TeboxMaliReclaim *r = opaque;

    /* Only GPA bitmap lives here — host munmap already ran in finalize. */
    if (r->m && r->m->hm_used) {
        bitmap_clear(r->m->hm_used, r->hm_off / TEBOX_MALI_PAGE,
                     r->size / TEBOX_MALI_PAGE);
    }
    g_free(r);
}

static void tbxm_buf_finalize(Object *obj)
{
    TeboxMaliBuf *b = TEBOX_MALI_BUF(obj);
    TeboxMaliReclaim *r = g_new(TeboxMaliReclaim, 1);

    /*
     * finalize runs after the FlatView RCU grace period, so no KVM slot
     * references ram_ptr anymore. Munmap immediately so kbase drops CSF
     * USER_IO / SAME_VA mappings (deferred munmap left NO_USER_FREE set and
     * spammed "freeing by user space is forbidden").
     */
    if (b->host_ptr && b->host_ptr != MAP_FAILED) {
        munmap(b->host_ptr, b->size);
        b->host_ptr = NULL;
    }
    r->m = b->m;
    r->hm_off = b->hm_off;
    r->size = b->size;
    r->host_ptr = NULL;
    b->m = NULL;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), tbxm_reclaim_bh, r);
}

static const TypeInfo tebox_mali_buf_info = {
    .parent = TYPE_OBJECT,
    .name = TYPE_TEBOX_MALI_BUF,
    .instance_size = sizeof(TeboxMaliBuf),
    .instance_finalize = tbxm_buf_finalize,
};

/* True if any live hostmem subregion covers [off, off+size). */
static bool tbxm_hm_subregion_overlap(TeboxMali *m, uint64_t off, uint64_t size)
{
    MemoryRegion *mr;
    uint64_t end = off + size;

    QTAILQ_FOREACH(mr, &m->hostmem.subregions, subregions_link) {
        uint64_t a = mr->addr;
        uint64_t b = a + int128_get64(mr->size);

        if (a < end && b > off) {
            return true;
        }
    }
    return false;
}

/*
 * Drop any live BAR MemoryRegion that covers [off, off+size). Catches
 * bitmap/MR desync that otherwise KVM_SET_USER_MEMORY_REGION EEXIST-aborts.
 */
static void tbxm_hm_evict_overlap(TeboxMali *m, uint64_t off, uint64_t size)
{
    MemoryRegion *mr, *next;
    uint64_t end = off + size;

    QTAILQ_FOREACH_SAFE(mr, &m->hostmem.subregions, subregions_link, next) {
        uint64_t a = mr->addr;
        uint64_t b = a + int128_get64(mr->size);

        if (!(a < end && b > off)) {
            continue;
        }
        for (int i = 0; i < TEBOX_MALI_MAX_MAPS; i++) {
            TeboxMaliMap *mp = &m->map[i];
            if (mp->used && mp->buf && &mp->buf->mr == mr) {
                tbxm_warn_rl("BAR overlap evict handle=%d [0x%" PRIx64
                             ",0x%" PRIx64 ") for new [0x%" PRIx64 ",0x%" PRIx64
                             ")", i + 1, a, b, off, end);
                tbxm_unmap_slot(m, mp);
                break;
            }
        }
    }
}

/* First-fit page run in the hostmem window; -1 if none. */
static int64_t tbxm_hm_alloc(TeboxMali *m, uint64_t size)
{
    unsigned long total = m->hostmem_size / TEBOX_MALI_PAGE;
    unsigned long n = size / TEBOX_MALI_PAGE;
    unsigned long start = 0;

again:
    while (start + n <= total) {
        start = find_next_zero_bit(m->hm_used, total, start);
        if (start + n > total) {
            break;
        }
        unsigned long busy = find_next_bit(m->hm_used, start + n, start);
        if (busy >= start + n) {
            uint64_t off = (uint64_t)start * TEBOX_MALI_PAGE;
            uint64_t sz = (uint64_t)n * TEBOX_MALI_PAGE;

            /*
             * hm_used bits stay set until RCU reclaim of a dead MR. If a
             * subregion is still attached over a "free" hole, force-evict
             * (desync) and retry — never add_subregion into a live GPA.
             */
            if (tbxm_hm_subregion_overlap(m, off, sz)) {
                tbxm_hm_evict_overlap(m, off, sz);
                start = busy + 1;
                goto again;
            }
            bitmap_set(m->hm_used, start, n);
            return (int64_t)off;
        }
        start = busy + 1;
    }
    return -1;
}

static void tbxm_unmap_slot(TeboxMali *m, TeboxMaliMap *mp)
{
    void *host_ptr;
    uint64_t size;

    if (!mp->used) {
        return;
    }
    if (mp->buf) {
        TeboxMaliBuf *b = mp->buf;
        /*
         * Leave hm_used bits set until tbxm_buf_finalize → reclaim BH.
         * Clearing them here races KVM's RCU tear-down of the old slot and
         * aborts with KVM_SET_USER_MEMORY_REGION EEXIST on the next mmap.
         */
        bitmap_set(m->hm_used, b->hm_off / TEBOX_MALI_PAGE,
                   b->size / TEBOX_MALI_PAGE);
        memory_region_del_subregion(&m->hostmem, &b->mr);
        /*
         * Munmap the kbase VMA now (not in RCU finalize). Deferred munmap
         * left CSF USER_IO bound so MEM_FREE(ring) hit NO_USER_FREE, and
         * SAME_VA holes stayed occupied → "Enclosing region not found" /
         * mmap -12 under AnTuTu. Keep a PROT_NONE placeholder at the same
         * VA so ram_ptr stays valid until FlatView RCU drops the KVM slot.
         */
        if (b->host_ptr && b->host_ptr != MAP_FAILED) {
            void *p = b->host_ptr;
            size_t sz = b->size;

            munmap(p, sz);
            void *ph = mmap(p, sz, PROT_NONE,
                            MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
            b->host_ptr = (ph == MAP_FAILED) ? NULL : ph;
        }
        mp->buf = NULL;
        memset(mp, 0, sizeof(*mp));
        object_unparent(OBJECT(b));
        return;
    }
    host_ptr = mp->host_ptr;
    size = mp->size;
    memset(mp, 0, sizeof(*mp));
    if (host_ptr && host_ptr != MAP_FAILED) {
        munmap(host_ptr, size);
    }
}

/* Rate-limited warning: a guest retry storm must not flood stderr (that alone
 * pegs QEMU at 100%). */
static void G_GNUC_PRINTF(1, 2) tbxm_warn_rl(const char *fmt, ...)
{
    static int64_t last_ms;
    static unsigned suppressed;
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    va_list ap;

    if (now - last_ms < 1000) {
        suppressed++;
        return;
    }
    last_ms = now;
    va_start(ap, fmt);
    g_autofree char *msg = g_strdup_vprintf(fmt, ap);
    va_end(ap);
    warn_report("tebox-mali: %s%s", msg,
                suppressed ? " (+ suppressed repeats)" : "");
    suppressed = 0;
}

/* ---- EVT queue: poll() wakeups and fence signals ------------------------- */

static void tbxm_evt_flush(TeboxMali *m)
{
    bool pushed = false;

    while (m->evt_pending->len) {
        VirtQueueElement *elem = virtqueue_pop(m->evt_vq,
                                               sizeof(VirtQueueElement));
        if (!elem) {
            break;      /* resumed when the guest posts buffers */
        }
        struct tebox_mali_event *ev =
            &g_array_index(m->evt_pending, struct tebox_mali_event, 0);
        size_t n = iov_from_buf(elem->in_sg, elem->in_num, 0, ev, sizeof(*ev));
        virtqueue_push(m->evt_vq, elem, n);
        g_free(elem);
        g_array_remove_index(m->evt_pending, 0);
        pushed = true;
    }
    if (pushed) {
        virtio_notify(VIRTIO_DEVICE(m), m->evt_vq);
    }
}

static void tbxm_evt_post(TeboxMali *m, uint32_t type, uint32_t ctx,
                          uint64_t id, const void *data, uint32_t data_len)
{
    struct tebox_mali_event ev = { .type = type, .ctx = ctx, .id = id };

    if (data && data_len) {
        if (data_len > TEBOX_MALI_EVT_DATA) {
            data_len = TEBOX_MALI_EVT_DATA;
        }
        ev.data_len = data_len;
        memcpy(ev.data, data, data_len);
    }
    g_array_append_val(m->evt_pending, ev);
    tbxm_evt_flush(m);
}

static void tebox_mali_handle_evt(VirtIODevice *vdev, VirtQueue *vq)
{
    (void)vq;
    tbxm_evt_flush(TEBOX_MALI(vdev));
}

static void tbxm_ctx_watch(TeboxMaliCtx *c, bool on);

/* An EVT_READABLE can go unanswered (the guest woke nobody), and with the
 * watch off the pending host event would then never be signaled again. */
#define TBXM_REARM_MS 20

static void tbxm_ctx_readable(void *opaque)
{
    TeboxMaliCtx *c = opaque;
    uint8_t buf[TEBOX_MALI_EVT_DATA];
    ssize_t n;
    int got = 0;

    /* Drain the host fd completely into piggybacked EVT payloads so the guest
     * rarely needs a follow-up OP_READ (each OP_READ is a full virtio/KVM
     * round-trip — the dominant cost when the screen moves). Level-triggered
     * poll would otherwise re-fire forever if we left the watch armed with
     * unread bytes. */
    tbxm_ctx_watch(c, false);
    while ((n = read(c->fd, buf, sizeof(buf))) > 0) {
        tbxm_evt_post(c->owner, TEBOX_MALI_EVT_READABLE, c->id, 0, buf,
                      (uint32_t)n);
        got++;
        /* Cap one wakeup so a runaway fd can't monopolize the QEMU thread;
         * the re-armed watch picks up the rest. */
        if (got >= 16) {
            break;
        }
    }
    if (got > 0) {
        tbxm_ctx_watch(c, true);
        return;
    }
    /* Spurious POLLIN or raced EAGAIN: wake guest pollers once, then rearm. */
    tbxm_evt_post(c->owner, TEBOX_MALI_EVT_READABLE, c->id, 0, NULL, 0);
    timer_mod(c->rearm, qemu_clock_get_ms(QEMU_CLOCK_REALTIME) + TBXM_REARM_MS);
}

static void tbxm_ctx_rearm(void *opaque)
{
    TeboxMaliCtx *c = opaque;

    if (c->used) {
        tbxm_ctx_watch(c, true);
    }
}

static void tbxm_ctx_watch(TeboxMaliCtx *c, bool on)
{
    if (c->watching == on || c->fd < 0) {
        return;
    }
    c->watching = on;
    qemu_set_fd_handler(c->fd, on ? tbxm_ctx_readable : NULL, NULL, c);
}

static void tbxm_fence_ready(void *opaque)
{
    TeboxMaliFence *f = opaque;
    uint32_t id = f->id;

    qemu_set_fd_handler(f->fd, NULL, NULL, NULL);
    close(f->fd);
    f->fd = -1;
    f->id = 0;
    tbxm_evt_post(f->owner, TEBOX_MALI_EVT_FENCE, 0, id, NULL, 0);
}

/* Take ownership of a host sync_file fd; returns the id the guest sees in
 * base_fence.basep.fd (positive int). */
static uint32_t tbxm_fence_add(TeboxMali *m, int fd)
{
    uint32_t id = ++m->next_fence_id;

    if (id > INT32_MAX) {
        id = m->next_fence_id = 1;
    }
    for (int i = 0; i < TEBOX_MALI_MAX_FENCES; i++) {
        TeboxMaliFence *f = &m->fence[i];
        if (f->fd < 0) {
            f->fd = fd;
            f->id = id;
            f->owner = m;
            qemu_set_fd_handler(fd, tbxm_fence_ready, NULL, f);
            return id;
        }
    }
    /* Table full: report it signaled now rather than leak or block. */
    tbxm_warn_rl("fence table full; signaling fence early");
    close(fd);
    tbxm_evt_post(m, TEBOX_MALI_EVT_FENCE, 0, id, NULL, 0);
    return id;
}

/* ---- op handlers: return 0 or -errno, fill *resp payload ----------------- */

static int tbxm_op_open(TeboxMali *m, void *out, uint32_t *out_len)
{
    int idx = -1;
    for (int i = 0; i < TEBOX_MALI_MAX_CTX; i++) {
        if (!m->ctx[i].used) { idx = i; break; }
    }
    if (idx < 0) {
        return -ENOSPC;
    }
    /* O_NONBLOCK so OP_READ (GPU completion events) never stalls the QEMU event
     * loop; EVT_READABLE gives the guest blocking read()/poll() semantics. */
    int fd = open(TEBOX_MALI_NODE, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd < 0) {
        return -errno;
    }
    m->ctx[idx].used = true;
    m->ctx[idx].fd = fd;
    m->ctx[idx].id = (uint32_t)idx;
    m->ctx[idx].owner = m;
    m->ctx[idx].watching = false;
    memset(m->ctx[idx].cookie_prot, 0, sizeof(m->ctx[idx].cookie_prot));
    memset(m->ctx[idx].heap_va, 0, sizeof(m->ctx[idx].heap_va));
    memset(m->ctx[idx].queue, 0, sizeof(m->ctx[idx].queue));
    tbxm_ctx_watch(&m->ctx[idx], true);
    struct tebox_mali_open_resp *r = out;
    r->ctx = (uint32_t)idx;
    *out_len = sizeof(*r);
    info_report("tebox-mali: ctx %d open %s (fd %d)", idx, TEBOX_MALI_NODE, fd);
    return 0;
}

static bool tbxm_is_user_io(const TeboxMaliMap *mp)
{
    return mp->pgoff >= TBXM_USER_IO_FIRST && mp->pgoff < TBXM_COOKIE_FIRST;
}

/*
 * Release every BAR mapping owned by |ctx| (also used at CLOSE). kbase only
 * defers region frees to context teardown when the munmapping mm is dying,
 * which QEMU's never is, so unmap the queues' USER_IO pages first: that
 * unbinds the queues, so their ring buffers are freeable (no "freeing by user
 * space is forbidden") and the GPU no longer reads them.
 */
static void tbxm_drop_ctx_maps(TeboxMali *m, int ctx)
{
    /* Context teardown must free immediately (no coalesce window). */
    memory_region_transaction_begin();
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < TEBOX_MALI_MAX_MAPS; i++) {
            TeboxMaliMap *mp = &m->map[i];
            if (mp->used && mp->ctx == ctx &&
                (pass || tbxm_is_user_io(mp))) {
                tbxm_unmap_slot(m, mp);
            }
        }
    }
    memory_region_transaction_commit();
}

/* Unmapping the cookie mapping frees the kbase region and unpins the pages,
 * so it must precede releasing the guest pages it points at. */
static void tbxm_import_release(TeboxMaliImport *im)
{
    munmap(im->gva, im->gva_size);
    if (im->buf_is_mmap) {
        munmap(im->buf, im->buf_size);
    } else {
        address_space_unmap(&address_space_memory, im->buf, im->buf_size,
                            true, 0);
    }
    memset(im, 0, sizeof(*im));
}

static TeboxMaliImport *tbxm_import_find(TeboxMali *m, int ctx, uint64_t gva)
{
    for (int i = 0; i < TEBOX_MALI_MAX_IMPORTS; i++) {
        TeboxMaliImport *im = &m->import[i];
        if (im->used && im->ctx == ctx && (uint64_t)(uintptr_t)im->gva == gva) {
            return im;
        }
    }
    return NULL;
}

static void tbxm_drop_ctx_imports(TeboxMali *m, int ctx)
{
    for (int i = 0; i < TEBOX_MALI_MAX_IMPORTS; i++) {
        if (m->import[i].used && m->import[i].ctx == ctx) {
            tbxm_import_release(&m->import[i]);
        }
    }
}

/* Remember the CPU access of a SAME_VA allocation by its cookie, so OP_MMAP
 * requests exactly what native libmali would (GPU-only memory -> PROT_NONE).
 * Accept any size that covers flags+gpu_va (16+): DDK packing of
 * MEM_ALLOC / MEM_ALLOC_EX has varied across r48/r49, and a strict 32/64
 * check left cookies unmarked — OP_MMAP then probed PROT_RW first and
 * kbase logged a flood of "mmap failed -1" for every GPU-only buffer. */
static void tbxm_note_alloc(TeboxMaliCtx *c, uint32_t cmd, const uint8_t *buf,
                            uint32_t size)
{
    uint32_t type = (cmd >> 8) & 0xff, nr = cmd & 0xff;
    uint64_t flags, gpu_va;

    if (type != TBXM_KBASE_IOCTL_TYPE || size < 16 ||
        (nr != TBXM_KBASE_NR_MEM_ALLOC && nr != TBXM_KBASE_NR_MEM_ALLOC_EX)) {
        return;
    }
    memcpy(&flags, buf, 8);         /* out.flags  */
    memcpy(&gpu_va, buf + 8, 8);    /* out.gpu_va */
    uint64_t pg = gpu_va >> 12;
    if ((gpu_va & 0xfff) || pg < TBXM_COOKIE_FIRST ||
        pg >= TBXM_COOKIE_FIRST + TBXM_NR_COOKIES) {
        return;                     /* not SAME_VA: real GPU VA, no cookie */
    }
    c->cookie_prot[pg - TBXM_COOKIE_FIRST] = TBXM_COOKIE_KNOWN |
        ((flags & TBXM_BASE_MEM_PROT_CPU_RD) ? PROT_READ : 0) |
        ((flags & TBXM_BASE_MEM_PROT_CPU_WR) ? PROT_WRITE : 0);
}

/* Track tiler heaps from successful TILER_HEAP_INIT/TERM. */
static void tbxm_note_heap(TeboxMaliCtx *c, uint32_t cmd, const uint8_t *buf,
                           uint32_t size)
{
    uint32_t type = (cmd >> 8) & 0xff, nr = cmd & 0xff;
    uint64_t va, from = 0, to;

    if (type != TBXM_KBASE_IOCTL_TYPE || size < 8) {
        return;
    }
    memcpy(&va, buf, 8);    /* INIT out.gpu_heap_va / TERM gpu_heap_va */
    if (nr == TBXM_KBASE_NR_HEAP_INIT && size >= 16) {
        to = va;
    } else if (nr == TBXM_KBASE_NR_HEAP_TERM && size == 8) {
        from = va;
        to = 0;
    } else {
        return;
    }
    for (int i = 0; i < TBXM_MAX_HEAPS; i++) {
        if (c->heap_va[i] == from) {
            c->heap_va[i] = to;
            return;
        }
    }
    if (to) {
        tbxm_warn_rl("ctx %u: more than %d tiler heaps", c->id, TBXM_MAX_HEAPS);
    }
}

static int tbxm_queue_slot(TeboxMaliCtx *c, uint64_t ring_va)
{
    for (int i = 0; i < TBXM_MAX_QUEUES; i++) {
        if (c->queue[i].ring_va == ring_va) {
            return i;
        }
    }
    return -1;
}

static void tbxm_queue_forget(TeboxMaliCtx *c, uint64_t ring_va)
{
    int i = tbxm_queue_slot(c, ring_va);
    if (i >= 0) {
        c->queue[i].ring_va = 0;
        c->queue[i].user_io_cookie = 0;
    }
}

/* Terminate one CSF queue by its ring buffer GPU VA (kbase base_addr). */
static void tbxm_queue_terminate_va(TeboxMaliCtx *c, uint64_t ring_va)
{
    if (!ring_va || c->fd < 0) {
        return;
    }
    uint64_t va = ring_va;
    ioctl(c->fd, _IOW(TBXM_KBASE_IOCTL_TYPE, TBXM_KBASE_NR_QUEUE_TERMINATE, va),
          &va);
    tbxm_queue_forget(c, ring_va);
}

/* Update CSF queue tracking after a successful ioctl (buf = post-ioctl struct). */
static void tbxm_note_queue(TeboxMaliCtx *c, uint32_t cmd, const uint8_t *buf,
                            uint32_t size)
{
    uint32_t type = (cmd >> 8) & 0xff, nr = cmd & 0xff;
    uint64_t ring;

    if (type != TBXM_KBASE_IOCTL_TYPE || !buf) {
        return;
    }
    if ((nr == TBXM_KBASE_NR_QUEUE_REGISTER && size >= 8) ||
        (nr == TBXM_KBASE_NR_QUEUE_REGISTER_EX && size >= 8)) {
        memcpy(&ring, buf, 8);
        if (!ring) {
            return;
        }
        for (int i = 0; i < TBXM_MAX_QUEUES; i++) {
            if (c->queue[i].ring_va == ring || c->queue[i].ring_va == 0) {
                c->queue[i].ring_va = ring;
                c->queue[i].user_io_cookie = 0;
                return;
            }
        }
        tbxm_warn_rl("ctx %u: more than %d CSF queues", c->id, TBXM_MAX_QUEUES);
        return;
    }
    if (nr == TBXM_KBASE_NR_QUEUE_TERMINATE && size >= 8) {
        memcpy(&ring, buf, 8);
        tbxm_queue_forget(c, ring);
    }
}

/* Remember BIND's USER_IO cookie for |ring_va|. mmap_handle is a byte file
 * offset (pgoff << PAGE_SHIFT) or already a pgoff — normalize to pgoff. */
static void tbxm_note_queue_bind(TeboxMaliCtx *c, uint64_t ring_va,
                                 uint64_t mmap_handle)
{
    int i = tbxm_queue_slot(c, ring_va);
    uint64_t cookie = mmap_handle;

    if (cookie >= TEBOX_MALI_PAGE) {
        cookie >>= 12;
    }
    if (i < 0) {
        /* REGISTER was missed; still record so MUNMAP/MEM_FREE can terminate. */
        for (i = 0; i < TBXM_MAX_QUEUES; i++) {
            if (c->queue[i].ring_va == 0) {
                c->queue[i].ring_va = ring_va;
                break;
            }
        }
        if (i >= TBXM_MAX_QUEUES) {
            return;
        }
    }
    c->queue[i].user_io_cookie = cookie;
}

/*
 * CSF queues put NO_USER_FREE on the ring buffer until QUEUE_TERMINATE(ring_va).
 * Only ioctl here — never del_subregion / drain_call_rcu from the virtio-notify
 * path (re-entrant MR + RCU stall).
 */
static void tbxm_terminate_csf_queues(TeboxMali *m, TeboxMaliCtx *c, int ctx)
{
    (void)m;
    (void)ctx;
    for (int i = 0; i < TBXM_MAX_QUEUES; i++) {
        if (c->queue[i].ring_va) {
            tbxm_queue_terminate_va(c, c->queue[i].ring_va);
        }
    }
}

static void tbxm_csf_terminate(TeboxMali *m, TeboxMaliCtx *c, int ctx)
{
    for (int h = 0; h <= UINT8_MAX; h++) {
        struct { uint8_t handle, pad[7]; } gt = { .handle = (uint8_t)h };
        ioctl(c->fd, _IOW(TBXM_KBASE_IOCTL_TYPE, TBXM_KBASE_NR_GROUP_TERMINATE,
                          gt), &gt);
    }
    for (int i = 0; i < TBXM_MAX_HEAPS; i++) {
        if (c->heap_va[i]) {
            ioctl(c->fd, _IOW(TBXM_KBASE_IOCTL_TYPE, TBXM_KBASE_NR_HEAP_TERM,
                              c->heap_va[i]), &c->heap_va[i]);
            c->heap_va[i] = 0;
        }
    }
    tbxm_terminate_csf_queues(m, c, ctx);
}

static int tbxm_op_close(TeboxMali *m, uint32_t ctx)
{
    TeboxMaliCtx *c = tbxm_ctx(m, ctx);
    if (!c) {
        return -EBADF;
    }
    tbxm_csf_terminate(m, c, (int)ctx);
    tbxm_drop_ctx_maps(m, (int)ctx);
    tbxm_drop_ctx_imports(m, (int)ctx);
    timer_del(c->rearm);
    tbxm_ctx_watch(c, false);
    close(c->fd);
    c->used = false;
    c->fd = -1;
    return 0;
}

/* Generic kbase ioctl forward: the request's flat struct rides in out_sg after
 * the headers; the (possibly modified) struct is written back to in_sg. */
static int tbxm_op_ioctl(TeboxMali *m, uint32_t ctx,
                         const struct tebox_mali_ioctl_req *req,
                         VirtQueueElement *elem, void *out, uint32_t *out_len)
{
    TeboxMaliCtx *c = tbxm_ctx(m, ctx);
    if (!c) {
        return -EBADF;
    }
    (void)out;
    uint32_t size = req->size;
    uint32_t n_ind = req->n_ind;
    if (size > TEBOX_MALI_IOCTL_MAX || n_ind > TEBOX_MALI_MAX_IND) {
        return -EMSGSIZE;
    }
    uint8_t buf[TEBOX_MALI_IOCTL_MAX];
    struct tebox_mali_ioctl_ind inds[TEBOX_MALI_MAX_IND];
    void *hbuf[TEBOX_MALI_MAX_IND] = { 0 };
    int ret;
    memset(buf, 0, size);

    /* out_sg: [cmd_hdr][ioctl_req][struct(size)][n_ind*ind][IN data...] */
    size_t off = sizeof(struct tebox_mali_cmd_hdr) + sizeof(*req);
    if (size &&
        iov_to_buf(elem->out_sg, elem->out_num, off, buf, size) != size) {
        return -EINVAL;
    }
    off += size;
    if (n_ind &&
        iov_to_buf(elem->out_sg, elem->out_num, off, inds,
                   n_ind * sizeof(inds[0])) != n_ind * sizeof(inds[0])) {
        return -EINVAL;
    }
    off += n_ind * sizeof(inds[0]);

    /* Relocate each embedded guest pointer to a host buffer; nested pointers
     * live in an earlier descriptor's buffer. */
    for (uint32_t i = 0; i < n_ind; i++) {
        uint32_t par = inds[i].parent;
        uint8_t *base = par ? hbuf[par - 1] : buf;
        uint32_t blen = par ? inds[par - 1].len : size;

        if (par > i || inds[i].len > (1u << 20) ||
            (uint64_t)inds[i].field_off + 8 > blen ||
            ((inds[i].dir & TEBOX_MALI_IND_FENCE_OUT) && inds[i].len < 4)) {
            ret = -EINVAL;
            goto out_free;
        }
        hbuf[i] = g_malloc0(inds[i].len ? inds[i].len : 1);
        if (inds[i].dir & TEBOX_MALI_IND_IN) {
            iov_to_buf(elem->out_sg, elem->out_num, off, hbuf[i], inds[i].len);
            off += inds[i].len;
        }
        uint64_t p = (uint64_t)(uintptr_t)hbuf[i];
        memcpy(base + inds[i].field_off, &p, sizeof(p));
    }

    /* Never munmap on MEM_FREE: the guest may still have the BAR pages mapped
     * and a vCPU touching a GPA whose host pages vanished dies with "kvm run
     * failed Bad address". Host mappings go away only on OP_MUNMAP, which the
     * guest sends after its VMA (and PTEs) are gone. */

    /* QUEUE_BIND overlays out.mmap_handle on in.buffer_gpu_addr — save ring. */
    uint64_t bind_ring = 0;
    bool is_bind = ((req->cmd >> 8) & 0xff) == TBXM_KBASE_IOCTL_TYPE &&
                   (req->cmd & 0xff) == TBXM_KBASE_NR_QUEUE_BIND && size >= 16;
    if (is_bind) {
        memcpy(&bind_ring, buf, 8);
    }

    /* libmali frees an import by MEM_FREE(gpu_va), but kbase refuses that for
     * SAME_VA memory; dropping our cookie mapping is what frees it. */
    if (((req->cmd >> 8) & 0xff) == TBXM_KBASE_IOCTL_TYPE &&
        (req->cmd & 0xff) == TBXM_KBASE_NR_MEM_FREE && size == 8) {
        uint64_t gva;
        memcpy(&gva, buf, 8);
        TeboxMaliImport *im = tbxm_import_find(m, (int)ctx, gva);
        if (im) {
            tbxm_import_release(im);
            *out_len = size;
            iov_from_buf(elem->in_sg, elem->in_num,
                         sizeof(struct tebox_mali_cmd_resp), buf, size);
            ret = 0;
            goto out_free;
        }
        /*
         * Do NOT QUEUE_TERMINATE from MEM_FREE. Libmali orders TERMINATE
         * itself; injecting it here raced with in-flight CSF work and
         * produced host page-fault@0 / CS_RESOURCE_TERMINATED / "queue not
         * bound", with AnTuTu scores collapsing ~90%. Ring NO_USER_FREE is
         * cleared on guest QUEUE_TERMINATE or ctx close (tbxm_csf_terminate).
         */
    }

    int r = tbxm_ioctl(c->fd, req->cmd, size ? buf : NULL);
    if (tbxm_trace()) {
        info_report("tebox-mali: ctx %u ioctl cmd=0x%x nr=%u size=%u n_ind=%u -> %d",
                    ctx, req->cmd, req->cmd & 0xff, size, n_ind, r);
    }
    {
        /* Once per (nr, errno) failure and once per nr returning > 0 (a
         * host fd number would be meaningless in the guest). */
        static uint8_t seen[256][2];
        unsigned nr = req->cmd & 0xff;
        int e = r < 0 ? errno : 0;
        int k = r < 0 ? 0 : 1;
        if ((r < 0 && seen[nr][0] != (uint8_t)e) || (r > 0 && !seen[nr][1])) {
            seen[nr][k] = r < 0 ? (uint8_t)e : 1;
            warn_report("tebox-mali: ioctl nr=%u size=%u n_ind=%u -> %d%s%s",
                        nr, size, n_ind, r, r < 0 ? " " : "",
                        r < 0 ? strerror(e) : "");
            errno = e;
        }
    }
    if (r < 0) {
        if (tbxm_trace()) {
            info_report("tebox-mali: ctx %u ioctl cmd=0x%x size=%u n_ind=%u -> errno %d (%s)",
                        ctx, req->cmd, size, n_ind, errno, strerror(errno));
        }
        ret = -errno;
        goto out_free;
    }
    tbxm_note_alloc(c, req->cmd, buf, size);
    tbxm_note_heap(c, req->cmd, buf, size);
    tbxm_note_queue(c, req->cmd, buf, size);
    if (is_bind && bind_ring && r >= 0) {
        uint64_t mmap_handle;
        memcpy(&mmap_handle, buf, 8); /* out.mmap_handle */
        tbxm_note_queue_bind(c, bind_ring, mmap_handle);
    }

    /* Host fds mean nothing in the guest. STREAM_CREATE's timeline fd is
     * unused on CSF (the guest hands out a placeholder); KCPU FENCE_SIGNAL
     * fds become fence ids signaled over the EVT queue. */
    if (((req->cmd >> 8) & 0xff) == TBXM_KBASE_IOCTL_TYPE &&
        (req->cmd & 0xff) == TBXM_KBASE_NR_STREAM_CREATE && r > 0) {
        close(r);
        r = 0;
    }
    for (uint32_t i = 0; i < n_ind; i++) {
        int32_t fd;

        if (!(inds[i].dir & TEBOX_MALI_IND_FENCE_OUT)) {
            continue;
        }
        memcpy(&fd, hbuf[i], 4);
        if (fd >= 0) {
            int32_t id = (int32_t)tbxm_fence_add(m, fd);
            memcpy(hbuf[i], &id, 4);
        }
    }

    /* Response payload written straight into in_sg after the cmd_resp:
     * [struct(size)][OUT buffers...]. The struct is shipped back as the kernel
     * left it: for _IOW ioctls (GET_GPUPROPS) the guest doesn't read it back,
     * and for union _IOWR ioctls (GLB_IFACE) the output overlays the pointer
     * fields, so the host addresses are already overwritten. */
    size_t roff = sizeof(struct tebox_mali_cmd_resp);
    if (size) {
        iov_from_buf(elem->in_sg, elem->in_num, roff, buf, size);
        roff += size;
    }
    for (uint32_t i = 0; i < n_ind; i++) {
        if (inds[i].dir & TEBOX_MALI_IND_OUT) {
            roff += iov_from_buf(elem->in_sg, elem->in_num, roff,
                                 hbuf[i], inds[i].len);
        }
    }
    *out_len = (uint32_t)(roff - sizeof(struct tebox_mali_cmd_resp));
    ret = r;   /* propagate the ioctl's own return value (>=0) */
out_free:
    for (uint32_t i = 0; i < n_ind; i++) {
        g_free(hbuf[i]);
    }
    return ret;
}

/*
 * mmap |off| of the kbase fd with the region's own CPU access |want| (from
 * MEM_ALLOC), or probe when unknown (-1: CSF USER_IO/USER_REG handles, aliases).
 * kbase rejects any VM_READ/VM_WRITE the region lacks with EPERM ("inconsistent
 * VM flags"), so GPU-only memory only maps PROT_NONE. RW is probed before RO:
 * PROT_READ also succeeds on writable regions, and the guest would then lose
 * write access it legitimately has.
 */
static void *tbxm_kbase_mmap(int fd, off_t off, size_t size, int want,
                             int *prot_out)
{
    /* Known want: try it alone when it is PROT_NONE (GPU-only). Falling
     * through to PROT_RW on a GPU-only region makes kbase printk
     * "mmap failed -1" once per buffer. For CPU-capable regions, keep a
     * short R/W → R → NONE ladder as a probe when want is unknown (-1). */
    static const int ladder[] = { PROT_READ | PROT_WRITE, PROT_READ, PROT_NONE };
    int tries[ARRAY_SIZE(ladder) + 1], n = 0, e = EINVAL;

    if (want == PROT_NONE) {
        tries[n++] = PROT_NONE;
    } else if (want >= 0) {
        tries[n++] = want;
        for (size_t i = 0; i < ARRAY_SIZE(ladder); i++) {
            if (ladder[i] != want) {
                tries[n++] = ladder[i];
            }
        }
    } else {
        for (size_t i = 0; i < ARRAY_SIZE(ladder); i++) {
            tries[n++] = ladder[i];
        }
    }
    for (int i = 0; i < n; i++) {
        void *p = mmap(NULL, size, tries[i], MAP_SHARED, fd, off);
        if (p != MAP_FAILED) {
            *prot_out = tries[i];
            return p;
        }
        e = errno;
        if (e != EPERM && e != EACCES && e != EINVAL) {
            break;
        }
    }
    errno = e;
    return MAP_FAILED;
}

static int tbxm_op_mmap(TeboxMali *m, uint32_t ctx,
                        const struct tebox_mali_mmap_req *req,
                        void *out, uint32_t *out_len)
{
    TeboxMaliCtx *c = tbxm_ctx(m, ctx);
    if (!c) {
        return -EBADF;
    }
    uint64_t size = ROUND_UP(req->size, TEBOX_MALI_PAGE);
    if (!size || size > m->hostmem_size) {
        return -EINVAL;
    }
    int idx = -1;
    for (int i = 0; i < TEBOX_MALI_MAX_MAPS; i++) {
        if (!m->map[i].used) { idx = i; break; }
    }
    if (idx < 0) {
        tbxm_warn_rl("out of map slots (ctx %u)", ctx);
        return -ENOMEM;
    }

    uint64_t pg = req->pgoff;
    uint8_t *cookie = NULL;
    int want = -1;
    if (pg >= TBXM_COOKIE_FIRST && pg < TBXM_COOKIE_FIRST + TBXM_NR_COOKIES) {
        cookie = &c->cookie_prot[pg - TBXM_COOKIE_FIRST];
        if (*cookie & TBXM_COOKIE_KNOWN) {
            want = *cookie & (PROT_READ | PROT_WRITE);
        }
    } else if (pg == TBXM_USER_REG_PG) {
        want = PROT_READ;   /* kbase rejects VM_WRITE with EPERM + dmesg */
    }

    /* Reserve BAR window space up front unless the buffer is GPU-only. */
    int64_t hm_off = -1;
    if (want != PROT_NONE) {
        hm_off = tbxm_hm_alloc(m, size);
        if (hm_off < 0) {
            tbxm_warn_rl("out of hostmem window (ctx %u, need %" PRIu64
                         " bytes of %" PRIu64 ")", ctx, size, m->hostmem_size);
            return -ENOMEM;
        }
    }
    int host_prot = PROT_NONE;
    void *p = tbxm_kbase_mmap(c->fd, (off_t)(pg << 12), (size_t)size, want,
                              &host_prot);
    if (p == MAP_FAILED || host_prot == PROT_NONE) {
        if (hm_off >= 0) {
            bitmap_clear(m->hm_used, hm_off / TEBOX_MALI_PAGE,
                         size / TEBOX_MALI_PAGE);
            hm_off = -1;
        }
    }
    if (p == MAP_FAILED) {
        int e = errno;
        tbxm_warn_rl("ctx %u mmap pgoff=0x%" PRIx64 " size=%" PRIu64
                     " want=%d failed: %s", ctx, pg, size, want, strerror(e));
        return -e;
    }
    if (cookie) {
        *cookie = 0;    /* consumed: kbase recycles the cookie */
    }

    TeboxMaliMap *mp = &m->map[idx];
    mp->used = true;
    mp->ctx = (int)ctx;
    mp->host_ptr = p;
    mp->pgoff = pg;
    mp->size = size;
    mp->host_prot = host_prot;
    mp->buf = NULL;
    if (hm_off >= 0) {
        static uint64_t buf_seq;
        TeboxMaliBuf *b = g_new0(TeboxMaliBuf, 1);
        g_autofree char *name = g_strdup_printf("tm-buf-%" PRIu64, ++buf_seq);

        object_initialize_child(OBJECT(m), name, b, TYPE_TEBOX_MALI_BUF);
        b->m = m;
        b->hm_off = (uint64_t)hm_off;
        b->size = size;
        b->host_ptr = p;
        memory_region_init_ram_ptr(&b->mr, OBJECT(b), "mr", size, p);
        /* A writable KVM slot over a PROT_READ host mapping turns the first
         * guest store into "kvm run failed Bad address"; make it ROM. */
        if (!(host_prot & PROT_WRITE)) {
            memory_region_set_readonly(&b->mr, true);
        }
        /* Final guard: never add into a GPA KVM still owns. */
        if (tbxm_hm_subregion_overlap(m, (uint64_t)hm_off, size)) {
            tbxm_hm_evict_overlap(m, (uint64_t)hm_off, size);
        }
        memory_region_add_subregion(&m->hostmem, b->hm_off, &b->mr);
        mp->hm_off = b->hm_off;
        mp->buf = b;
    }

    if (tbxm_trace()) {
        info_report("tebox-mali: ctx %u mmap pgoff=0x%" PRIx64 " size=%" PRIu64
                    " want=%d -> host %p prot=%d hm_off=%s0x%" PRIx64
                    " handle=%d", ctx, pg, size, want, p, host_prot,
                    mp->buf ? "" : "(none)", mp->hm_off, idx + 1);
    }

    struct tebox_mali_mmap_resp *r = out;
    memset(r, 0, sizeof(*r));
    r->hostmem_offset = mp->hm_off;
    r->size = size;
    r->gpu_va = (uint64_t)(uintptr_t)p;   /* host map addr == GPU VA (SAME_VA) */
    r->host_prot = (uint32_t)host_prot;
    r->flags = TEBOX_MALI_MMAP_F_PROT;
    r->handle = (uint32_t)(idx + 1);
    *out_len = sizeof(*r);
    return 0;
}

static int tbxm_op_munmap(TeboxMali *m, uint32_t ctx,
                          const struct tebox_mali_munmap_req *req)
{
    TeboxMaliCtx *c = tbxm_ctx(m, ctx);
    uint32_t h = req->handle;
    if (!h || h > TEBOX_MALI_MAX_MAPS) {
        return -EINVAL;
    }
    TeboxMaliMap *mp = &m->map[h - 1];
    if (!mp->used || mp->ctx != (int)ctx) {
        return -ENOENT;
    }
    /*
     * Do NOT QUEUE_TERMINATE from USER_IO munmap. Guest GL rebinds queues
     * while the ring is still live; terminating here panicked host kbase.
     * NO_USER_FREE clears when this unmap_slot munmaps USER_IO (kbase
     * vm_close → unbind), on guest QUEUE_TERMINATE, or on ctx close.
     */
    (void)c;
    tbxm_unmap_slot(m, mp);
    return 0;
}

/* OP_READ: forward a read() of the kbase fd (GPU completion / CSF event
 * records). The fd is O_NONBLOCK so this never stalls the QEMU event loop;
 * -EAGAIN means no event pending and the guest shim retries. */
static int tbxm_op_read(TeboxMali *m, uint32_t ctx,
                        const struct tebox_mali_read_req *req,
                        void *out, uint32_t *out_len)
{
    TeboxMaliCtx *c = tbxm_ctx(m, ctx);
    if (!c) {
        return -EBADF;
    }
    uint32_t cap = req->max_len;
    if (cap > TEBOX_MALI_IOCTL_MAX) {
        cap = TEBOX_MALI_IOCTL_MAX;
    }
    /* Fill the guest buffer in one OP_READ: libmali often asks for a large
     * read after poll; returning a single 32-byte record forces another
     * virtio round-trip for every CSF notification. */
    uint32_t got = 0;
    int err = 0;
    while (got < cap) {
        ssize_t n = read(c->fd, (uint8_t *)out + got, cap - got);
        if (n < 0) {
            err = errno;
            break;
        }
        if (n == 0) {
            break;
        }
        got += (uint32_t)n;
    }
    tbxm_ctx_watch(c, true);
    if (got == 0 && err) {
        return -err;
    }
    *out_len = got;
    return (int)got;
}

/* kbase MEM_IMPORT (USER_BUFFER) — minimal local ABI, see mali_kbase_ioctl.h. */
#define TBXM_KBASE_IOCTL_MEM_IMPORT  0xc0188016u  /* _IOWR(0x80,22,24) */
#define TBXM_BASE_MEM_IMPORT_TYPE_USER_BUFFER 3
#define TBXM_BASE_MEM_CACHED_CPU     (1u << 12)   /* kbase: USER_BUFFER must be CPU cached */
#define TBXM_BASE_MEM_COHERENT_SYSTEM (1u << 10)  /* GPU writes snoop CPU caches     */
struct tbxm_base_mem_import_user_buffer { uint64_t ptr; uint64_t length; };
union tbxm_kbase_ioctl_mem_import {
    struct { uint64_t flags, phandle; uint32_t type, padding; } in;
    struct { uint64_t flags, gpu_va, va_pages; } out;
};

/* Upper bound on page segments in one import; the guest sends them inline in a
 * single ~32K command buffer (2048 * 16 bytes fits), so this also bounds the
 * contiguous host VA we stitch together. */
#define TBXM_IMPORT_MAX_SEG 2048

/* Remap one guest-physical segment's pages (from the memfd that backs guest
 * RAM) into the host VA range at |dst|, so scattered guest pages become one
 * contiguous host VA for a single kbase USER_BUFFER import. Guest RAM must be
 * memfd-backed (boot-qemu.sh adds -object memory-backend-memfd under
 * TEBOX_MALI); anonymous RAM has no fd to remap from. 0 on success. */
static int tbxm_remap_seg_from_memfd(uint64_t gpa, uint64_t len, void *dst)
{
    hwaddr mlen = len;
    void *hva = address_space_map(&address_space_memory, gpa, &mlen, true,
                                  MEMTXATTRS_UNSPECIFIED);
    if (!hva || mlen < len) {
        if (hva) {
            address_space_unmap(&address_space_memory, hva, mlen, true, 0);
        }
        warn_report("tebox-mali: import seg gpa 0x%" PRIx64 " len %" PRIu64
                    " map failed (got %" PRIu64 ")", gpa, len, (uint64_t)mlen);
        return -EFAULT;
    }
    ram_addr_t ra;
    RAMBlock *rb = qemu_ram_block_from_host(hva, false, &ra);
    int ret = -EFAULT;
    if (rb) {
        int fd = qemu_ram_get_fd(rb);
        if (fd < 0) {
            warn_report("tebox-mali: scattered import needs memfd-backed RAM "
                        "(guest RAM has no fd)");
        } else {
            off_t fd_off = (off_t)rb->fd_offset +
                           ((uint8_t *)hva - (uint8_t *)qemu_ram_get_host_addr(rb));
            void *p = mmap(dst, len, PROT_READ | PROT_WRITE,
                           MAP_SHARED | MAP_FIXED, fd, fd_off);
            if (p == dst) {
                ret = 0;
            } else {
                warn_report("tebox-mali: import seg mmap(memfd) -> errno %d (%s)",
                            errno, strerror(errno));
            }
        }
    }
    /* We now hold the pages via the memfd mapping (or failed); drop the
     * address_space ref either way. */
    address_space_unmap(&address_space_memory, hva, mlen, true, 0);
    return ret;
}

/* OP_IMPORT: the stock Mali blob imports a guest dma-heap dmabuf into kbase; the
 * guest resolves it to guest-physical page segments. We assemble those pages
 * into one contiguous host VA and import it into host kbase as a USER_BUFFER, so
 * the real GPU DMAs the guest buffer's pages in place (zero copy). A single
 * contiguous segment maps directly; a scattered buffer (minigbm gives
 * SurfaceFlinger's render targets as non-contiguous guest pages) is stitched
 * from the memfd backing into one contiguous host VA. */
static int tbxm_op_import(TeboxMali *m, uint32_t ctx,
                          const struct tebox_mali_import_req *req,
                          VirtQueueElement *elem, void *out, uint32_t *out_len)
{
    TeboxMaliCtx *c = tbxm_ctx(m, ctx);
    if (!c) {
        return -EBADF;
    }
    uint32_t n_seg = req->n_seg;
    if (n_seg < 1 || n_seg > TBXM_IMPORT_MAX_SEG) {
        warn_report("tebox-mali: import n_seg=%u out of range", n_seg);
        return -EINVAL;
    }
    const size_t seg_off = sizeof(struct tebox_mali_cmd_hdr) + sizeof(*req);

    /* Total buffer length = sum of the segment lengths. */
    uint64_t total = 0;
    for (uint32_t i = 0; i < n_seg; i++) {
        struct tebox_mali_import_seg s;
        if (iov_to_buf(elem->out_sg, elem->out_num, seg_off + i * sizeof(s),
                       &s, sizeof(s)) != sizeof(s)) {
            return -EINVAL;
        }
        total += s.len;
    }

    void *buf;          /* contiguous host VA covering the whole guest buffer */
    bool buf_is_mmap;   /* cleanup: munmap(buf) vs address_space_unmap        */
    hwaddr single_mlen = 0;

    if (n_seg == 1) {
        /* One segment is already contiguous in host VA (works for anonymous
         * RAM too, so the non-scattered path keeps no memfd dependency). */
        struct tebox_mali_import_seg s;
        if (iov_to_buf(elem->out_sg, elem->out_num, seg_off, &s, sizeof(s)) !=
            sizeof(s)) {
            return -EINVAL;
        }
        single_mlen = s.len;
        buf = address_space_map(&address_space_memory, s.gpa, &single_mlen, true,
                                MEMTXATTRS_UNSPECIFIED);
        if (!buf || single_mlen < s.len) {
            if (buf) {
                address_space_unmap(&address_space_memory, buf, single_mlen,
                                    true, 0);
            }
            warn_report("tebox-mali: import gpa 0x%" PRIx64 " len %" PRIu64
                        " map failed (got %" PRIu64 ")", s.gpa,
                        (uint64_t)s.len, (uint64_t)single_mlen);
            return -EFAULT;
        }
        buf_is_mmap = false;
    } else {
        /* Scattered: reserve one contiguous VA and remap each guest page
         * segment (from the memfd backing) into it, in buffer order. */
        buf = mmap(NULL, total, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (buf == MAP_FAILED) {
            return -ENOMEM;
        }
        uint8_t *cur = buf;
        for (uint32_t i = 0; i < n_seg; i++) {
            struct tebox_mali_import_seg s;
            if (iov_to_buf(elem->out_sg, elem->out_num, seg_off + i * sizeof(s),
                           &s, sizeof(s)) != sizeof(s)) {
                munmap(buf, total);
                return -EINVAL;
            }
            if (tbxm_remap_seg_from_memfd(s.gpa, s.len, cur)) {
                munmap(buf, total);
                return -EFAULT;
            }
            cur += s.len;
        }
        buf_is_mmap = true;
    }
    TeboxMaliImport *im = NULL;
    for (int i = 0; i < TEBOX_MALI_MAX_IMPORTS; i++) {
        if (!m->import[i].used) {
            im = &m->import[i];
            break;
        }
    }
    if (!im) {
        tbxm_warn_rl("import table full");
        if (buf_is_mmap) {
            munmap(buf, total);
        } else {
            address_space_unmap(&address_space_memory, buf, single_mlen, true, 0);
        }
        return -ENOMEM;
    }

    struct tbxm_base_mem_import_user_buffer ub = {
        .ptr = (uint64_t)(uintptr_t)buf, .length = total };
    union tbxm_kbase_ioctl_mem_import u;
    memset(&u, 0, sizeof(u));
    /* kbase requires USER_BUFFER imports to be CPU-cached; COHERENT_SYSTEM so
     * GPU writes snoop the CPU caches (guest CPU sees them without flushes). */
    u.in.flags = req->flags | TBXM_BASE_MEM_CACHED_CPU |
                 TBXM_BASE_MEM_COHERENT_SYSTEM;
    u.in.phandle = (uint64_t)(uintptr_t)&ub;
    u.in.type = TBXM_BASE_MEM_IMPORT_TYPE_USER_BUFFER;
    if (tbxm_ioctl(c->fd, TBXM_KBASE_IOCTL_MEM_IMPORT, &u)) {
        int e = errno;
        tbxm_warn_rl("ctx %u MEM_IMPORT user_buffer n_seg=%u len %" PRIu64
                     " -> errno %d (%s)", ctx, n_seg, total, e, strerror(e));
        if (buf_is_mmap) {
            munmap(buf, total);
        } else {
            address_space_unmap(&address_space_memory, buf, single_mlen, true, 0);
        }
        return -e;
    }
    /* A 64-bit USER_BUFFER import is forced into the SAME_VA zone with
     * BASE_MEM_NEED_MMAP, so out.gpu_va is a COOKIE; mmap it to make kbase
     * insert the pinned pages into the GPU page tables and realize the GPU VA
     * (SAME_VA: the returned host VA equals the GPU VA). The guest uses that
     * GPU VA directly in GPU commands; its CPU side stays the guest dma-heap
     * mapping. */
    uint64_t cookie = u.out.gpu_va;
    size_t msize = (size_t)u.out.va_pages * TEBOX_MALI_PAGE;
    void *gva = mmap(NULL, msize, PROT_READ | PROT_WRITE, MAP_SHARED,
                     c->fd, (off_t)cookie);
    if (gva == MAP_FAILED) {
        int e = errno;
        info_report("tebox-mali: ctx %u import mmap(cookie 0x%" PRIx64
                    ") -> errno %d (%s)", ctx, cookie, e, strerror(e));
        uint64_t free_cookie = cookie;
        tbxm_ioctl(c->fd, _IOW(TBXM_KBASE_IOCTL_TYPE, TBXM_KBASE_NR_MEM_FREE,
                               uint64_t), &free_cookie);
        if (buf_is_mmap) {
            munmap(buf, total);
        } else {
            address_space_unmap(&address_space_memory, buf, single_mlen, true, 0);
        }
        return -e;
    }
    *im = (TeboxMaliImport) {
        .used = true, .ctx = (int)ctx, .gva = gva, .gva_size = msize,
        .buf = buf, .buf_size = buf_is_mmap ? total : single_mlen,
        .buf_is_mmap = buf_is_mmap,
    };

    struct tebox_mali_import_resp *r = out;
    r->gpu_va = (uint64_t)(uintptr_t)gva;   /* realized GPU VA (== host VA) */
    r->va_pages = u.out.va_pages;
    *out_len = sizeof(*r);
    if (tbxm_trace()) {
        info_report("tebox-mali: ctx %u import n_seg=%u total %" PRIu64
                    " -> cookie 0x%" PRIx64 " realized gpu_va 0x%" PRIx64,
                    ctx, n_seg, total, cookie, r->gpu_va);
    }
    return 0;
}

/* ---- CMD virtqueue ------------------------------------------------------- */

static void tebox_mali_handle_cmd(VirtIODevice *vdev, VirtQueue *vq)
{
    TeboxMali *m = TEBOX_MALI(vdev);

    /* Do NOT wrap this loop in memory_region_transaction_*: mmap must publish
     * KVM memslots before a later ioctl in the same kick touches the BAR. */
    for (;;) {
        VirtQueueElement *elem = virtqueue_pop(vq, sizeof(VirtQueueElement));
        if (!elem) {
            break;
        }
        struct tebox_mali_cmd_hdr hdr;
        struct tebox_mali_cmd_resp resp = { 0 };
        uint8_t outbuf[TEBOX_MALI_IOCTL_MAX];
        uint32_t payload = 0;
        bool wrote_in_sg = false;   /* op already wrote its payload into in_sg */

        if (iov_to_buf(elem->out_sg, elem->out_num, 0, &hdr, sizeof(hdr)) !=
            sizeof(hdr)) {
            resp.result = -EINVAL;
            goto reply;
        }
        resp.seq = hdr.seq;

        /* Bring-up profiling: ops/sec by type (+ top ioctl nr), flushed once a
         * second to a file. ioctl/read dominate KVM exits; mmap/munmap drive
         * flatview rebuilds. */
        {
            static uint32_t cnt[8];
            static uint32_t nr_cnt[256];
            static int64_t next_us;
            if (hdr.op < 8) {
                cnt[hdr.op]++;
            }
            if (hdr.op == TEBOX_MALI_OP_IOCTL &&
                elem->out_sg && elem->out_num) {
                struct tebox_mali_ioctl_req iq;
                if (iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr),
                               &iq, sizeof(iq)) == sizeof(iq)) {
                    nr_cnt[iq.cmd & 0xff]++;
                }
            }
            int64_t now = g_get_monotonic_time();
            if (now >= next_us) {
                FILE *f = fopen("/tmp/tbxmali.log", "a");
                if (f) {
                    unsigned top_nr = 0, top_n = 0;
                    for (unsigned i = 0; i < 256; i++) {
                        if (nr_cnt[i] > top_n) {
                            top_n = nr_cnt[i];
                            top_nr = i;
                        }
                    }
                    fprintf(f, "ops/s: ioctl=%u mmap=%u munmap=%u import=%u "
                            "read=%u open=%u close=%u top_nr=%u:%u\n",
                            cnt[TEBOX_MALI_OP_IOCTL], cnt[TEBOX_MALI_OP_MMAP],
                            cnt[TEBOX_MALI_OP_MUNMAP], cnt[TEBOX_MALI_OP_IMPORT],
                            cnt[TEBOX_MALI_OP_READ], cnt[TEBOX_MALI_OP_OPEN],
                            cnt[TEBOX_MALI_OP_CLOSE], top_nr, top_n);
                    fclose(f);
                }
                memset(cnt, 0, sizeof(cnt));
                memset(nr_cnt, 0, sizeof(nr_cnt));
                next_us = now + 1000000;
            }
        }

        switch (hdr.op) {
        case TEBOX_MALI_OP_OPEN:
            resp.result = tbxm_op_open(m, outbuf, &payload);
            break;
        case TEBOX_MALI_OP_CLOSE:
            resp.result = tbxm_op_close(m, hdr.ctx);
            break;
        case TEBOX_MALI_OP_IOCTL: {
            struct tebox_mali_ioctl_req req;
            if (iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), &req,
                           sizeof(req)) != sizeof(req)) {
                resp.result = -EINVAL;
                break;
            }
            resp.result = tbxm_op_ioctl(m, hdr.ctx, &req, elem, outbuf, &payload);
            wrote_in_sg = true;   /* op_ioctl wrote [struct][OUT] into in_sg */
            break;
        }
        case TEBOX_MALI_OP_MMAP: {
            struct tebox_mali_mmap_req req;
            if (iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), &req,
                           sizeof(req)) != sizeof(req)) {
                resp.result = -EINVAL;
                break;
            }
            resp.result = tbxm_op_mmap(m, hdr.ctx, &req, outbuf, &payload);
            break;
        }
        case TEBOX_MALI_OP_MUNMAP: {
            struct tebox_mali_munmap_req req;
            if (iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), &req,
                           sizeof(req)) != sizeof(req)) {
                resp.result = -EINVAL;
                break;
            }
            resp.result = tbxm_op_munmap(m, hdr.ctx, &req);
            break;
        }
        case TEBOX_MALI_OP_IMPORT: {
            struct tebox_mali_import_req req;
            if (iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), &req,
                           sizeof(req)) != sizeof(req)) {
                resp.result = -EINVAL;
                break;
            }
            resp.result = tbxm_op_import(m, hdr.ctx, &req, elem, outbuf, &payload);
            break;
        }
        case TEBOX_MALI_OP_READ: {
            struct tebox_mali_read_req req;
            if (iov_to_buf(elem->out_sg, elem->out_num, sizeof(hdr), &req,
                           sizeof(req)) != sizeof(req)) {
                resp.result = -EINVAL;
                break;
            }
            resp.result = tbxm_op_read(m, hdr.ctx, &req, outbuf, &payload);
            break;
        }
        default:
            resp.result = -EINVAL;
        }

    reply:
        resp.len = payload;
        {
            size_t off = iov_from_buf(elem->in_sg, elem->in_num, 0, &resp,
                                      sizeof(resp));
            if (payload && !wrote_in_sg) {
                iov_from_buf(elem->in_sg, elem->in_num, off, outbuf, payload);
            }
            virtqueue_push(vq, elem, sizeof(resp) + payload);
        }
        g_free(elem);
    }
    virtio_notify(vdev, vq);
}

/* ---- device realize ------------------------------------------------------ */

static void tebox_mali_realize(DeviceState *dev, Error **errp)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    TeboxMali *m = TEBOX_MALI(dev);

    virtio_init(vdev, TEBOX_MALI_VIRTIO_ID, 0);
    m->cmd_vq = virtio_add_queue(vdev, 128, tebox_mali_handle_cmd);
    m->evt_vq = virtio_add_queue(vdev, 256, tebox_mali_handle_evt);
    m->evt_pending = g_array_new(false, false, sizeof(struct tebox_mali_event));
    for (int i = 0; i < TEBOX_MALI_MAX_FENCES; i++) {
        m->fence[i].fd = -1;
    }
    if (!m->hostmem_size) {
        m->hostmem_size = TEBOX_MALI_HOSTMEM_DEFAULT;
    }
    m->hm_used = bitmap_new(m->hostmem_size / TEBOX_MALI_PAGE);
    /* The hostmem window MemoryRegion + its PCI BAR are set up by the PCI
     * proxy's realize (before this device realizes), like virtio-gpu, so the
     * virtio modern MMIO can be relocated off BAR 4 first. */
    for (int i = 0; i < TEBOX_MALI_MAX_CTX; i++) {
        m->ctx[i].fd = -1;
        m->ctx[i].rearm = timer_new_ms(QEMU_CLOCK_REALTIME, tbxm_ctx_rearm,
                                       &m->ctx[i]);
    }
    info_report("tebox-mali: virtio device ready (id=%d, hostmem=%" PRIu64 " MiB)",
                TEBOX_MALI_VIRTIO_ID, m->hostmem_size >> 20);
}

static void tebox_mali_unrealize(DeviceState *dev)
{
    VirtIODevice *vdev = VIRTIO_DEVICE(dev);
    TeboxMali *m = TEBOX_MALI(dev);
    for (int i = 0; i < TEBOX_MALI_MAX_CTX; i++) {
        if (m->ctx[i].used) {
            tbxm_drop_ctx_maps(m, i);
            tbxm_drop_ctx_imports(m, i);
            tbxm_ctx_watch(&m->ctx[i], false);
            close(m->ctx[i].fd);
        }
        timer_free(m->ctx[i].rearm);
    }
    for (int i = 0; i < TEBOX_MALI_MAX_FENCES; i++) {
        if (m->fence[i].fd >= 0) {
            qemu_set_fd_handler(m->fence[i].fd, NULL, NULL, NULL);
            close(m->fence[i].fd);
            m->fence[i].fd = -1;
        }
    }
    g_array_free(m->evt_pending, true);
    m->evt_pending = NULL;
    g_free(m->hm_used);
    m->hm_used = NULL;
    virtio_delete_queue(m->evt_vq);
    virtio_delete_queue(m->cmd_vq);
    virtio_cleanup(vdev);
}

static uint64_t tebox_mali_get_features(VirtIODevice *vdev, uint64_t features,
                                        Error **errp)
{
    (void)vdev; (void)errp;
    return features;
}

static const VMStateDescription tebox_mali_vmstate = {
    .name = "tebox-mali",
    .unmigratable = 1,
};

static const Property tebox_mali_props[] = {
    DEFINE_PROP_SIZE("hostmem", TeboxMali, hostmem_size, TEBOX_MALI_HOSTMEM_DEFAULT),
};

static void tebox_mali_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioDeviceClass *vdc = VIRTIO_DEVICE_CLASS(klass);
    (void)data;

    device_class_set_props(dc, tebox_mali_props);
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    vdc->realize = tebox_mali_realize;
    vdc->unrealize = tebox_mali_unrealize;
    vdc->get_features = tebox_mali_get_features;
    dc->vmsd = &tebox_mali_vmstate;
}

static const TypeInfo tebox_mali_info = {
    .name = TYPE_TEBOX_MALI,
    .parent = TYPE_VIRTIO_DEVICE,
    .instance_size = sizeof(TeboxMali),
    .class_init = tebox_mali_class_init,
};

/* ---- virtio-pci proxy (adds the hostmem BAR + shared-memory cap) --------- */

#define TYPE_TEBOX_MALI_PCI "tebox-mali-pci-base"
typedef struct TeboxMaliPCI TeboxMaliPCI;
DECLARE_INSTANCE_CHECKER(TeboxMaliPCI, TEBOX_MALI_PCI, TYPE_TEBOX_MALI_PCI)

struct TeboxMaliPCI {
    VirtIOPCIProxy parent_obj;
    TeboxMali vdev;
};

static void tebox_mali_pci_realize(VirtIOPCIProxy *vpci_dev, Error **errp)
{
    TeboxMaliPCI *dev = TEBOX_MALI_PCI(vpci_dev);
    DeviceState *vdev = DEVICE(&dev->vdev);
    uint64_t sz = dev->vdev.hostmem_size ? dev->vdev.hostmem_size
                                         : TEBOX_MALI_HOSTMEM_DEFAULT;

    /* Free BAR 4 for the hostmem window (as virtio-gpu does): relocate the
     * virtio modern MMIO and MSI-X off it, init the window, register the BAR +
     * shared-memory cap, THEN realize so the virtio caps land on the relocated
     * bars. The guest shim finds the window via the shm region and remaps
     * kbase buffers onto it. */
    vpci_dev->msix_bar_idx = 1;
    vpci_dev->modern_mem_bar_idx = 2;
    /* One MSI-X vector per queue plus config. Without vectors the guest falls
     * back to shared INTx, where every completion costs an ISR read that
     * contends for the BQL with the main loop running the kbase ioctl. */
    vpci_dev->nvectors = TEBOX_MALI_VQ_MAX + 1;
    memory_region_init(&dev->vdev.hostmem, OBJECT(&dev->vdev),
                       "tebox-mali-hostmem", sz);
    pci_register_bar(&vpci_dev->pci_dev, TEBOX_MALI_HOSTMEM_BAR,
                     PCI_BASE_ADDRESS_SPACE_MEMORY |
                     PCI_BASE_ADDRESS_MEM_PREFETCH |
                     PCI_BASE_ADDRESS_MEM_TYPE_64,
                     &dev->vdev.hostmem);
    virtio_pci_add_shm_cap(vpci_dev, TEBOX_MALI_HOSTMEM_BAR, 0, sz,
                           TEBOX_MALI_SHM_ID_HOSTMEM);

    qdev_realize(vdev, BUS(&vpci_dev->bus), errp);
}

static void tebox_mali_pci_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    VirtioPCIClass *k = VIRTIO_PCI_CLASS(klass);
    PCIDeviceClass *pcidev = PCI_DEVICE_CLASS(klass);
    (void)data;

    k->realize = tebox_mali_pci_realize;
    set_bit(DEVICE_CATEGORY_MISC, dc->categories);
    pcidev->vendor_id = PCI_VENDOR_ID_REDHAT_QUMRANET;
    pcidev->device_id = 0x1040 + TEBOX_MALI_VIRTIO_ID;  /* modern virtio-pci */
    pcidev->revision = VIRTIO_PCI_ABI_VERSION;
    pcidev->class_id = PCI_CLASS_OTHERS;
}

static void tebox_mali_pci_instance_init(Object *obj)
{
    TeboxMaliPCI *dev = TEBOX_MALI_PCI(obj);
    virtio_instance_init_common(obj, &dev->vdev, sizeof(dev->vdev),
                                TYPE_TEBOX_MALI);
}

static const VirtioPCIDeviceTypeInfo tebox_mali_pci_info = {
    .base_name = TYPE_TEBOX_MALI_PCI,
    .generic_name = "tebox-mali-pci",
    .instance_size = sizeof(TeboxMaliPCI),
    .instance_init = tebox_mali_pci_instance_init,
    .class_init = tebox_mali_pci_class_init,
};

static void tebox_mali_register_types(void)
{
    type_register_static(&tebox_mali_buf_info);
    type_register_static(&tebox_mali_info);
    virtio_pci_types_register(&tebox_mali_pci_info);
}

type_init(tebox_mali_register_types)
