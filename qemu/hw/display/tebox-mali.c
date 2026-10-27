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
 * Memory plane:
 *   Default: contiguous /dev/mali0 VMAs share one growable KVM slot
 *   (qemu_ram_resize; host pointer stays put). A buffer that is not exactly
 *   adjacent, or that does not fit the reservation, keeps a per-buffer MR.
 *   TEBOX_MALI_WIN_IMPORT stays off — USER_BUFFER import is not the boot path.
 */
#include "qemu/osdep.h"
#include "qemu/bitmap.h"
#include "qemu/error-report.h"
#include "qemu/iov.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/mmap-alloc.h"
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
#include <sys/syscall.h>
#ifndef MREMAP_MAYMOVE
#define MREMAP_MAYMOVE 1
#endif
#ifndef MREMAP_FIXED
#define MREMAP_FIXED 2
#endif
#ifndef MFD_CLOEXEC
#define MFD_CLOEXEC 0x0001U
#endif

#include "tebox-mali-proto.h"

static int tbxm_memfd_create(const char *name, unsigned int flags)
{
#ifdef __NR_memfd_create
    return (int)syscall(__NR_memfd_create, name, flags);
#else
    (void)name;
    (void)flags;
    errno = ENOSYS;
    return -1;
#endif
}

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
/* Single KVM slot for >=4 MiB CPU allocs. gfx/rutabaga use ~512M; a 2–16 GiB
 * background made later dmabuf import mmap(cookie) return ENOMEM. */
#define TBXM_WIN_BYTES (512ull << 20)
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
#define TBXM_BASE_MEM_GROW_ON_GPF    (1ull << 9)
#define TBXM_BASE_MEM_COHERENT_SYSTEM (1ull << 10)
#define TBXM_BASE_MEM_CACHED_CPU     (1ull << 12) /* USER_BUFFER must be CPU-cached */
#define TBXM_BASE_MEM_SAME_VA        (1ull << 13)
#define TBXM_BASE_MEM_NEED_MMAP      (1ull << 14)
#define TBXM_USER_REG_PG             0x2f  /* BASEP_MEM_CSF_USER_REG_PAGE_HANDLE */
#define TBXM_USER_IO_FIRST           0x30  /* BASEP_MEM_CSF_USER_IO_PAGES_HANDLE */
#define TBXM_COOKIE_FIRST            0x40  /* BASE_MEM_COOKIE_BASE >> PAGE_SHIFT */
#define TBXM_NR_COOKIES              64
#define TBXM_COOKIE_KNOWN            0x80  /* cookie_prot[] entry is valid      */
#define TBXM_WIN_OFF_NONE            UINT64_MAX  /* win_off[]: no pending import */

/* kbase MEM_IMPORT (USER_BUFFER) — see mali_kbase_ioctl.h. */
#define TBXM_KBASE_IOCTL_MEM_IMPORT  0xc0188016u  /* _IOWR(0x80,22,24) */
#define TBXM_BASE_MEM_IMPORT_TYPE_USER_BUFFER 3
struct tbxm_base_mem_import_user_buffer { uint64_t ptr; uint64_t length; };
union tbxm_kbase_ioctl_mem_import {
    struct { uint64_t flags, phandle; uint32_t type, padding; } in;
    struct { uint64_t flags, gpu_va, va_pages; } out;
};

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

/* MAP_FIXED into hostmem window: hit/miss (miss → slow per-buffer MR). */
static uint64_t tbxm_fixed_ok, tbxm_fixed_miss;
static int tbxm_fixed_miss_errno;
/* Window + USER_BUFFER alloc fast path (gfx-like one-slot model). */
static uint64_t tbxm_win_ok, tbxm_win_fail;

/* Opt-in: try MAP_FIXED of mali0 into the window (CIX kbase always EINVAL). */
static bool tbxm_env_map_fixed(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("TEBOX_MALI_MAP_FIXED");
        cached = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

/*
 * Opt-in: hostmem_background + window USER_BUFFER alloc fast path.
 * Default OFF — a 16 GiB background under the BAR made dmabuf import
 * mmap(cookie) return ENOMEM and SurfaceFlinger crash-loop.
 */
static bool tbxm_env_win_import(void)
{
    static int cached = -1;
    if (cached < 0) {
        const char *e = getenv("TEBOX_MALI_WIN_IMPORT");
        cached = (e && e[0] && e[0] != '0') ? 1 : 0;
    }
    return cached == 1;
}

/*
 * Native-path split for buffers >= 4 MiB. alloc_ns is the kbase MEM_ALLOC
 * ioctl only; mmap_ns is the kbase mmap; slot_ns is everything after that
 * (zone grow, or a per-buffer add_subregion). zone_grow counts resizes of
 * an existing slot; zone_new counts slots created; zone_skip is fallback.
 */
static struct {
    uint64_t n_alloc, alloc_ns;
    uint64_t n_mmap, mmap_ns, slot_ns;
    uint64_t n_grow, n_down, n_zone_new, n_zone_skip;
} tbxm_prof;

static uint64_t tbxm_prof_now(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
}

static void tbxm_prof_report(void)
{
    info_report("tebox-mali: prof alloc n=%" PRIu64 " avg_us=%" PRIu64
                " mmap n=%" PRIu64 " kbase_mmap_avg_us=%" PRIu64
                " kvm_slot_avg_us=%" PRIu64
                " zone_new=%" PRIu64 " zone_grow=%" PRIu64
                " zone_down=%" PRIu64 " zone_skip=%" PRIu64,
                tbxm_prof.n_alloc,
                tbxm_prof.n_alloc ? tbxm_prof.alloc_ns / tbxm_prof.n_alloc / 1000 : 0,
                tbxm_prof.n_mmap,
                tbxm_prof.n_mmap ? tbxm_prof.mmap_ns / tbxm_prof.n_mmap / 1000 : 0,
                tbxm_prof.n_mmap ? tbxm_prof.slot_ns / tbxm_prof.n_mmap / 1000 : 0,
                tbxm_prof.n_zone_new, tbxm_prof.n_grow, tbxm_prof.n_down,
                tbxm_prof.n_zone_skip);
}

/*
 * Slow-path only: per-buffer MemoryRegion (rutabaga-style). Used when
 * MAP_FIXED into the hostmem window fails. RCU-bound: hm_used stays set
 * until finalize/reclaim after del_subregion.
 */
#define TYPE_TEBOX_MALI_BUF "tebox-mali-buf"
struct TeboxMaliBuf {
    Object parent_obj;
    MemoryRegion mr;
    struct TeboxMali *m;
    uint64_t hm_off;
    uint64_t size;
    void *host_ptr;
    int memfd;              /* >=0 => shared memfd (win-import); -1 else */
    bool pooled;            /* sitting in buf_pool; MR still on the BAR */
    bool win_pages;         /* USER_BUFFER pool (not slow-path placeholder) */
};
OBJECT_DECLARE_SIMPLE_TYPE(TeboxMaliBuf, TEBOX_MALI_BUF)

/* Recycled per-buffer MRs: unmap keeps the KVM slot, next mmap MAP_FIXED's
 * into the same HVA — zero add/del_subregion on the hot free/alloc path. */
#define TBXM_BUF_POOL_MAX 512

typedef struct TeboxMaliReclaim {
    struct TeboxMali *m;
    uint64_t hm_off;
    uint64_t size;
    void *host_ptr;
} TeboxMaliReclaim;

/* One mapped kbase buffer / CS user-IO page block exposed into the BAR. */
typedef struct TeboxMaliMap {
    bool used;
    bool map_fixed;         /* true => background slot only (no per-buf MR) */
    bool win_import;        /* window pages + USER_BUFFER import fast path  */
    bool zone;              /* covered by a growable zone slot, no own MR */
    int ctx;
    void *host_ptr;         /* host mmap VA (== gpu_va for SAME_VA)         */
    uint64_t pgoff;         /* the kbase mmap cookie (gpu_va or handle)     */
    uint64_t size;
    uint64_t hm_off;        /* byte offset within the hostmem window        */
    int host_prot;          /* PROT_* of the host mmap                      */
    TeboxMaliBuf *buf;      /* non-NULL => slow-path live BAR MemoryRegion  */
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
    /* WIN_IMPORT: hm_off for cookies not yet mmap'd (UINT64_MAX = none). */
    uint64_t win_off[TBXM_NR_COOKIES];
    uint64_t win_size[TBXM_NR_COOKIES];
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

/*
 * Size-class freelist for the hostmem window (powers of two, 4 KiB .. 16 MiB).
 * Hot path: O(1) pop/push; overflow sizes fall back to bitmap first-fit.
 */
#define TBXM_FL_MIN_SHIFT  12   /* 4 KiB */
#define TBXM_FL_MAX_SHIFT  24   /* 16 MiB */
#define TBXM_FL_CLASSES    (TBXM_FL_MAX_SHIFT - TBXM_FL_MIN_SHIFT + 1)

typedef struct TeboxMaliFreeList {
    uint64_t *offs;         /* byte offsets into hostmem window */
    unsigned n;
    unsigned cap;
} TeboxMaliFreeList;

/*
 * One KVM slot, created once. kbase places the next VMA just below the
 * previous one, so the first >=4 MiB mapping sits near the top of an 8 GiB
 * window. The addresses below it are reserved only long enough to register
 * the slot, then released so kbase can map there. A later VMA inside the
 * window needs no KVM update. Anything else keeps a per-buffer MR.
 */
#define TBXM_ZONES           24
#define TBXM_ARENA_BYTES     (8ull << 30)
#define TBXM_ARENA_HEAD      (64ull << 20)

typedef struct TeboxMaliZone {
    bool used;
    MemoryRegion *mr;
    void *base;             /* HVA of the low end of the covered run     */
    uint64_t size;          /* bytes currently covered                   */
    uint64_t max;           /* BAR reservation bytes                     */
    uint64_t hm_base;       /* GPA of the reservation                    */
    uint64_t hm_off;        /* GPA of base                               */
} TeboxMaliZone;

struct TeboxMali {
    VirtIODevice parent_obj;
    VirtQueue *cmd_vq;
    VirtQueue *evt_vq;
    GArray *evt_pending;    /* struct tebox_mali_event awaiting a guest buffer */
    MemoryRegion hostmem;   /* the PCI BAR window (registered by the proxy) */
    MemoryRegion hostmem_background; /* single KVM slot over the whole window */
    void *hostmem_mmap;     /* HVA of hostmem_background (MAP_FIXED target) */
    MemoryRegion win_mr;    /* one KVM slot, TBXM_WIN_BYTES, top of the BAR  */
    void *win_base;         /* HVA of win_mr (memfd)                          */
    uint64_t win_hm_off;    /* BAR offset of win_mr                           */
    uint64_t win_size;
    int win_mfd;            /* -1 until the window is created                */
    unsigned long *win_used;/* one bit per page inside the window            */
    bool win_ready;
    TeboxMaliZone zone[TBXM_ZONES];
    bool arena_ready;
    MemoryRegion *arena_mr;
    void *arena_base;       /* HVA of the low end of the one slot         */
    uint64_t arena_size;
    uint64_t arena_hm;
    uint64_t hostmem_size;
    unsigned long *hm_used; /* one bit per window page                      */
    unsigned long hm_scan_cur; /* first-fit cursor (pages)                  */
    TeboxMaliFreeList freelist[TBXM_FL_CLASSES];
    TeboxMaliBuf *buf_pool[TBXM_BUF_POOL_MAX];
    unsigned buf_pool_n;
    uint32_t next_fence_id;
    /* Free map[] indices (O(1) instead of scanning 4096). */
    int map_free[TEBOX_MALI_MAX_MAPS];
    int map_free_n;
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
static bool tbxm_mr_is_zone(TeboxMali *m, MemoryRegion *mr);
static void G_GNUC_PRINTF(1, 2) tbxm_warn_rl(const char *fmt, ...);
static void tbxm_hm_free(TeboxMali *m, uint64_t off, uint64_t size);
static void *tbxm_kbase_mmap(int fd, off_t off, size_t size, int want,
                             void *fixed, int *prot_out, bool *used_fixed);
static int tbxm_op_mmap(TeboxMali *m, uint32_t ctx,
                        const struct tebox_mali_mmap_req *req,
                        void *out, uint32_t *out_len);

static void tbxm_map_slot_free(TeboxMali *m, int idx)
{
    if (m->map_free_n < TEBOX_MALI_MAX_MAPS) {
        m->map_free[m->map_free_n++] = idx;
    }
}

static int tbxm_map_slot_alloc(TeboxMali *m)
{
    if (m->map_free_n > 0) {
        return m->map_free[--m->map_free_n];
    }
    for (int i = 0; i < TEBOX_MALI_MAX_MAPS; i++) {
        if (!m->map[i].used) {
            return i;
        }
    }
    return -1;
}

static TeboxMaliBuf *tbxm_buf_pool_pop_kind(TeboxMali *m, uint64_t size,
                                            bool win_pages)
{
    for (unsigned i = 0; i < m->buf_pool_n; i++) {
        TeboxMaliBuf *b = m->buf_pool[i];
        if (b && b->size == size && b->win_pages == win_pages &&
            b->host_ptr && b->host_ptr != MAP_FAILED) {
            m->buf_pool[i] = m->buf_pool[--m->buf_pool_n];
            b->pooled = false;
            return b;
        }
    }
    return NULL;
}

static TeboxMaliBuf *tbxm_buf_pool_pop(TeboxMali *m, uint64_t size)
{
    return tbxm_buf_pool_pop_kind(m, size, false);
}

static bool tbxm_buf_pool_push(TeboxMali *m, TeboxMaliBuf *b)
{
    if (m->buf_pool_n >= TBXM_BUF_POOL_MAX || !b || !b->host_ptr) {
        return false;
    }
    b->pooled = true;
    m->buf_pool[m->buf_pool_n++] = b;
    return true;
}

static int tbxm_fl_class(uint64_t size)
{
    if (size < (1ull << TBXM_FL_MIN_SHIFT) ||
        size > (1ull << TBXM_FL_MAX_SHIFT) ||
        (size & (size - 1))) {
        return -1;
    }
    return (int)(ctz64(size) - TBXM_FL_MIN_SHIFT);
}

static void tbxm_fl_push(TeboxMali *m, uint64_t off, uint64_t size)
{
    int c = tbxm_fl_class(size);
    TeboxMaliFreeList *fl;

    if (c < 0) {
        bitmap_clear(m->hm_used, off / TEBOX_MALI_PAGE,
                     size / TEBOX_MALI_PAGE);
        return;
    }
    fl = &m->freelist[c];
    if (fl->n == fl->cap) {
        unsigned ncap = fl->cap ? fl->cap * 2 : 64;
        fl->offs = g_renew(uint64_t, fl->offs, ncap);
        fl->cap = ncap;
    }
    fl->offs[fl->n++] = off;
}

static int64_t tbxm_fl_pop(TeboxMali *m, uint64_t size)
{
    int c = tbxm_fl_class(size);
    TeboxMaliFreeList *fl;

    if (c < 0) {
        return -1;
    }
    fl = &m->freelist[c];
    if (!fl->n) {
        return -1;
    }
    return (int64_t)fl->offs[--fl->n];
}

static void tbxm_reclaim_bh(void *opaque)
{
    TeboxMaliReclaim *r = opaque;

    /* Slow-path only: GPA becomes reusable after RCU grace. */
    if (r->m && r->m->hm_used) {
        tbxm_hm_free(r->m, r->hm_off, r->size);
    }
    g_free(r);
}

static void tbxm_buf_finalize(Object *obj)
{
    TeboxMaliBuf *b = TEBOX_MALI_BUF(obj);
    TeboxMaliReclaim *r;

    /*
     * Slow-path finalize: FlatView RCU done. Drop placeholder; freelist via BH.
     * Rebind steals the GPA (b->m = NULL) so we must not hm_free it here.
     */
    if (b->host_ptr && b->host_ptr != MAP_FAILED) {
        munmap(b->host_ptr, b->size);
        b->host_ptr = NULL;
    }
    if (b->memfd >= 0) {
        close(b->memfd);
        b->memfd = -1;
    }
    if (!b->m) {
        return;
    }
    r = g_new(TeboxMaliReclaim, 1);
    r->m = b->m;
    r->hm_off = b->hm_off;
    r->size = b->size;
    r->host_ptr = NULL;
    b->m = NULL;
    aio_bh_schedule_oneshot(qemu_get_aio_context(), tbxm_reclaim_bh, r);
}

/*
 * Drop a pooled Buf's MemoryRegion without releasing its GPA. Caller reuses
 * hm_off for a fresh ram_ptr MR (MAP_FIXED into the old HVA failed).
 */
static uint64_t tbxm_buf_steal_gpa(TeboxMali *m, TeboxMaliBuf *b)
{
    uint64_t off = b->hm_off;
    void *p = b->host_ptr;
    size_t sz = b->size;

    /* Keep hm_used set — GPA stays owned across the del/re-add. */
    bitmap_set(m->hm_used, off / TEBOX_MALI_PAGE, sz / TEBOX_MALI_PAGE);
    /* Drop KVM slot while HVA is still mapped, then release the pages. */
    memory_region_del_subregion(&m->hostmem, &b->mr);
    b->host_ptr = NULL;
    b->m = NULL; /* finalize: no second munmap, no hm_free */
    if (p && p != MAP_FAILED) {
        munmap(p, sz);
    }
    object_unparent(OBJECT(b));
    return off;
}

static const TypeInfo tebox_mali_buf_info = {
    .parent = TYPE_OBJECT,
    .name = TYPE_TEBOX_MALI_BUF,
    .instance_size = sizeof(TeboxMaliBuf),
    .instance_finalize = tbxm_buf_finalize,
};

/* True if a slow-path (non-background) subregion covers [off, off+size). */
static bool tbxm_hm_subregion_overlap(TeboxMali *m, uint64_t off, uint64_t size)
{
    MemoryRegion *mr;
    uint64_t end = off + size;

    QTAILQ_FOREACH(mr, &m->hostmem.subregions, subregions_link) {
        uint64_t a, b;

        if (mr == &m->hostmem_background || mr == &m->win_mr ||
            tbxm_mr_is_zone(m, mr)) {
            continue;
        }
        a = mr->addr;
        b = a + int128_get64(mr->size);
        if (a < end && b > off) {
            return true;
        }
    }
    return false;
}

static void tbxm_hm_evict_overlap(TeboxMali *m, uint64_t off, uint64_t size)
{
    MemoryRegion *mr, *next;
    uint64_t end = off + size;

    QTAILQ_FOREACH_SAFE(mr, &m->hostmem.subregions, subregions_link, next) {
        uint64_t a, b;

        if (mr == &m->hostmem_background || mr == &m->win_mr ||
            tbxm_mr_is_zone(m, mr)) {
            continue;
        }
        a = mr->addr;
        b = a + int128_get64(mr->size);
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

static void tbxm_hm_free(TeboxMali *m, uint64_t off, uint64_t size)
{
    tbxm_fl_push(m, off, size);
}

/* Freelist hit or first-fit page run; -1 if none. */
static int64_t tbxm_hm_alloc(TeboxMali *m, uint64_t size)
{
    unsigned long total = m->hostmem_size / TEBOX_MALI_PAGE;
    unsigned long n = size / TEBOX_MALI_PAGE;
    unsigned long start;
    int64_t reused = tbxm_fl_pop(m, size);

    if (reused >= 0) {
        /* Slot was reserved when pushed; bits still set. */
        return reused;
    }
    /* Bump cursor: avoid re-scanning the whole 16 GiB bitmap from 0. */
    start = m->hm_scan_cur;
    if (start >= total) {
        start = 0;
    }

    for (int pass = 0; pass < 2; pass++) {
        unsigned long s = start;

        while (s + n <= total) {
            unsigned long gap = find_next_zero_bit(m->hm_used, total, s);
            unsigned long busy;

            if (gap + n > total) {
                break;
            }
            busy = find_next_bit(m->hm_used, gap + n, gap);
            if (busy >= gap + n) {
                uint64_t off = (uint64_t)gap * TEBOX_MALI_PAGE;
                uint64_t sz = (uint64_t)n * TEBOX_MALI_PAGE;

                if (tbxm_hm_subregion_overlap(m, off, sz)) {
                    tbxm_hm_evict_overlap(m, off, sz);
                    s = gap + 1;
                    continue;
                }
                bitmap_set(m->hm_used, gap, n);
                m->hm_scan_cur = gap + n;
                return (int64_t)off;
            }
            s = busy + 1;
        }
        if (start == 0) {
            break; /* already full scan */
        }
        start = 0; /* wrap once */
    }
    return -1;
}

/*
 * Hold a KVM-safe placeholder under a recycled HVA. PROT_NONE (not RW anon):
 * CIX kbase returns EINVAL for MAP_FIXED over RW pages in a large window;
 * a dedicated PROT_NONE VMA is the VirGL-like replace target we want to retry.
 */
static void tbxm_hm_restore_anon(void *addr, size_t size)
{
    void *p = mmap(addr, size, PROT_NONE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    if (p == MAP_FAILED) {
        /* Fallback: RW anon still keeps the memslot HVA valid. */
        p = mmap(addr, size, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
    }
    if (p == MAP_FAILED) {
        tbxm_warn_rl("hostmem restore placeholder at %p size=%zu failed: %s",
                     addr, size, strerror(errno));
    }
}

static void tbxm_unmap_slot(TeboxMali *m, TeboxMaliMap *mp)
{
    void *host_ptr;
    uint64_t size, hm_off;
    bool map_fixed;

    if (!mp->used) {
        return;
    }

    /* Background MAP_FIXED path (opt-in probe only). */
    if (mp->map_fixed && m->hostmem_mmap && !mp->win_import) {
        int map_idx = (int)(mp - m->map);
        void *win;

        host_ptr = mp->host_ptr;
        size = mp->size;
        hm_off = mp->hm_off;
        win = (uint8_t *)m->hostmem_mmap + hm_off;
        memset(mp, 0, sizeof(*mp));
        if (map_idx >= 0 && map_idx < TEBOX_MALI_MAX_MAPS) {
            tbxm_map_slot_free(m, map_idx);
        }
        if (host_ptr && host_ptr != MAP_FAILED) {
            munmap(host_ptr, size);
            tbxm_hm_restore_anon(host_ptr, (size_t)size);
        } else {
            tbxm_hm_restore_anon(win, (size_t)size);
        }
        tbxm_hm_free(m, hm_off, size);
        return;
    }

    /*
     * Zone member: the KVM slot stays (it may still cover neighbors). munmap
     * exactly this VMA and put a PROT_NONE placeholder back so the slot has
     * no hole. Do not del the zone MR and do not free its GPA reservation.
     */
    if (mp->zone) {
        int map_idx = (int)(mp - m->map);

        host_ptr = mp->host_ptr;
        size = mp->size;
        memset(mp, 0, sizeof(*mp));
        if (map_idx >= 0 && map_idx < TEBOX_MALI_MAX_MAPS) {
            tbxm_map_slot_free(m, map_idx);
        }
        if (host_ptr && host_ptr != MAP_FAILED && size) {
            munmap(host_ptr, size);
            tbxm_hm_restore_anon(host_ptr, (size_t)size);
        }
        return;
    }

    /*
     * Win-import: drop the cookie mmap only. The 512 MiB window is one KVM
     * slot and stays. Legacy per-buffer MRs (buf != NULL) return to buf_pool.
     */
    if (mp->win_import) {
        int map_idx = (int)(mp - m->map);
        TeboxMaliBuf *b = mp->buf;

        host_ptr = mp->host_ptr;
        size = mp->size;
        hm_off = mp->hm_off;
        mp->buf = NULL;
        memset(mp, 0, sizeof(*mp));
        if (map_idx >= 0 && map_idx < TEBOX_MALI_MAX_MAPS) {
            tbxm_map_slot_free(m, map_idx);
        }
        if (host_ptr && host_ptr != MAP_FAILED) {
            munmap(host_ptr, size); /* cookie VA; length == kbase VMA */
        }
        if (!b) {
            if (m->win_ready && size && hm_off >= m->win_hm_off &&
                hm_off + size <= m->win_hm_off + m->win_size) {
                bitmap_clear(m->win_used,
                             (hm_off - m->win_hm_off) / TEBOX_MALI_PAGE,
                             size / TEBOX_MALI_PAGE);
            }
            return;
        }
        if (tbxm_buf_pool_push(m, b)) {
            return;
        }
        /* Pool full: tear down the slot. */
        bitmap_set(m->hm_used, b->hm_off / TEBOX_MALI_PAGE,
                   b->size / TEBOX_MALI_PAGE);
        memory_region_del_subregion(&m->hostmem, &b->mr);
        object_unparent(OBJECT(b));
        return;
    }

    if (mp->buf) {
        TeboxMaliBuf *b = mp->buf;
        void *p = b->host_ptr;
        size_t sz = b->size;
        int map_idx = (int)(mp - m->map);

        /*
         * Hot recycle: keep the BAR MemoryRegion + KVM slot. Replace mali
         * pages with anon at the same HVA and park the buf in buf_pool so the
         * next same-size mmap can MAP_FIXED in-place (no del_subregion).
         */
        if (p && p != MAP_FAILED) {
            munmap(p, sz);
            tbxm_hm_restore_anon(p, sz);
            b->host_ptr = p; /* placeholder re-held at same HVA */
        }
        mp->buf = NULL;
        memset(mp, 0, sizeof(*mp));
        if (map_idx >= 0 && map_idx < TEBOX_MALI_MAX_MAPS) {
            tbxm_map_slot_free(m, map_idx);
        }
        if (b->host_ptr && tbxm_buf_pool_push(m, b)) {
            return;
        }
        /* Pool full or restore failed: tear down the KVM slot (slow). */
        bitmap_set(m->hm_used, b->hm_off / TEBOX_MALI_PAGE,
                   b->size / TEBOX_MALI_PAGE);
        memory_region_del_subregion(&m->hostmem, &b->mr);
        object_unparent(OBJECT(b));
        return;
    }
    host_ptr = mp->host_ptr;
    size = mp->size;
    map_fixed = mp->map_fixed;
    hm_off = mp->hm_off;
    memset(mp, 0, sizeof(*mp));
    if (host_ptr && host_ptr != MAP_FAILED) {
        munmap(host_ptr, size);
    }
    (void)map_fixed;
    (void)hm_off;
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
    memset(m->ctx[idx].win_off, 0xff, sizeof(m->ctx[idx].win_off));
    memset(m->ctx[idx].win_size, 0, sizeof(m->ctx[idx].win_size));
    memset(m->ctx[idx].heap_va, 0, sizeof(m->ctx[idx].heap_va));
    memset(m->ctx[idx].queue, 0, sizeof(m->ctx[idx].queue));
    tbxm_ctx_watch(&m->ctx[idx], true);
    struct tebox_mali_open_resp *r = out;
    r->ctx = (uint32_t)idx;
    *out_len = sizeof(*r);
    if (tbxm_trace()) {
        info_report("tebox-mali: ctx %d open %s (fd %d)", idx,
                    TEBOX_MALI_NODE, fd);
    }
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
/* Drop a pending WIN_IMPORT reservation (alloc'd but not yet mmap'd). */
static void tbxm_win_release_pending(TeboxMali *m, TeboxMaliCtx *c, int ci)
{
    uint64_t off, sz;

    if (!c || ci < 0 || ci >= TBXM_NR_COOKIES) {
        return;
    }
    if (c->win_off[ci] == TBXM_WIN_OFF_NONE) {
        return;
    }
    off = c->win_off[ci];
    sz = c->win_size[ci];
    c->win_off[ci] = TBXM_WIN_OFF_NONE;
    c->win_size[ci] = 0;
    if (sz && m->hm_used) {
        bitmap_clear(m->hm_used, off / TEBOX_MALI_PAGE,
                     sz / TEBOX_MALI_PAGE);
    }
}

static void tbxm_drop_ctx_win(TeboxMali *m, TeboxMaliCtx *c)
{
    if (!c) {
        return;
    }
    for (int i = 0; i < TBXM_NR_COOKIES; i++) {
        if (c->win_off[i] == TBXM_WIN_OFF_NONE) {
            continue;
        }
        /* Free the kbase import so cookies do not stick in the host DDK. */
        uint64_t cookie = ((uint64_t)(TBXM_COOKIE_FIRST + i)) << 12;
        tbxm_ioctl(c->fd, _IOW(TBXM_KBASE_IOCTL_TYPE, TBXM_KBASE_NR_MEM_FREE,
                               uint64_t), &cookie);
        tbxm_win_release_pending(m, c, i);
    }
}

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
    tbxm_drop_ctx_win(m, c);
    tbxm_drop_ctx_imports(m, (int)ctx);
    timer_del(c->rearm);
    tbxm_ctx_watch(c, false);
    close(c->fd);
    c->used = false;
    c->fd = -1;
    return 0;
}

/* Log why a >=2 MiB MEM_ALLOC stayed on the native path (first 16 only). */
static void tbxm_win_skip(const uint8_t *buf, uint32_t size, const char *why)
{
    static int n;
    uint64_t w0 = 0, w1 = 0, w2 = 0, w3 = 0;

    if (n >= 16 || size < 8) {
        return;
    }
    memcpy(&w0, buf, 8);
    if (w0 < 512) {
        return;
    }
    n++;
    if (size >= 16) {
        memcpy(&w1, buf + 8, 8);
    }
    if (size >= 24) {
        memcpy(&w2, buf + 16, 8);
    }
    if (size >= 32) {
        memcpy(&w3, buf + 24, 8);
    }
    info_report("tebox-mali: win-skip %s size=%u va=%" PRIu64
                " commit=%" PRIu64 " ext=0x%" PRIx64 " flags=0x%" PRIx64,
                why, size, w0, w1, w2, w3);
}

/* One KVM slot at the top of the BAR. Created on the first large alloc so
 * boot's dmabuf imports happen before this VMA exists. */
static int tbxm_win_ensure(TeboxMali *m)
{
    uint64_t sz = TBXM_WIN_BYTES;
    unsigned long total, n, start;
    uint64_t off;
    int mfd;
    void *p;

    if (m->win_ready) {
        return 0;
    }
    if (!sz || sz > m->hostmem_size || !m->hm_used) {
        return -1;
    }
    total = m->hostmem_size / TEBOX_MALI_PAGE;
    n = sz / TEBOX_MALI_PAGE;
    if (!n || n > total) {
        return -1;
    }
    start = total - n;
    if (find_next_bit(m->hm_used, start + n, start) < start + n ||
        tbxm_hm_subregion_overlap(m, (uint64_t)start * TEBOX_MALI_PAGE, sz)) {
        tbxm_warn_rl("win slot: top of BAR is busy");
        return -1;
    }
    off = (uint64_t)start * TEBOX_MALI_PAGE;
    mfd = tbxm_memfd_create("tebox-mali-win", MFD_CLOEXEC);
    if (mfd < 0 || ftruncate(mfd, (off_t)sz) < 0) {
        if (mfd >= 0) {
            close(mfd);
        }
        return -1;
    }
    /* Hint above 16 TiB so the VMA does not split kbase's mmap hole. */
    p = mmap((void *)(1ull << 44), (size_t)sz, PROT_READ | PROT_WRITE,
             MAP_SHARED, mfd, 0);
    if (p == MAP_FAILED) {
        close(mfd);
        return -1;
    }
    memory_region_init_ram_ptr(&m->win_mr, OBJECT(m), "tebox-mali-win", sz, p);
    memory_region_add_subregion(&m->hostmem, off, &m->win_mr);
    bitmap_set(m->hm_used, start, n);
    m->win_used = bitmap_new(n);
    m->win_base = p;
    m->win_hm_off = off;
    m->win_size = sz;
    m->win_mfd = mfd;
    m->win_ready = true;
    info_report("tebox-mali: win slot %" PRIu64 " MiB BAR 0x%" PRIx64 " hva=%p",
                sz >> 20, off, p);
    return 0;
}

static int64_t tbxm_win_slice_alloc(TeboxMali *m, uint64_t size)
{
    unsigned long total = m->win_size / TEBOX_MALI_PAGE;
    unsigned long n = size / TEBOX_MALI_PAGE;
    unsigned long s = 0;

    if (!m->win_used || !n || n > total) {
        return -1;
    }
    while (s + n <= total) {
        unsigned long gap = find_next_zero_bit(m->win_used, total, s);
        unsigned long busy;

        if (gap + n > total) {
            break;
        }
        busy = find_next_bit(m->win_used, gap + n, gap);
        if (busy >= gap + n) {
            bitmap_set(m->win_used, gap, n);
            return (int64_t)gap * TEBOX_MALI_PAGE;
        }
        s = busy + 1;
    }
    return -1;
}

/*
 * >=4 MiB SAME_VA CPU alloc: USER_BUFFER-import a slice of the one window
 * slot. No per-buffer add_subregion. The ioctl reply still carries the
 * cookie plus NEED_MMAP|SAME_VA so libmali mmaps; eager mmap_resp carries
 * the realized GPU VA. Returns 0, or 1 to fall through to native MEM_ALLOC.
 */
static int tbxm_win_import_alloc(TeboxMali *m, TeboxMaliCtx *c, uint32_t ctx,
                                 uint8_t *buf, uint32_t size,
                                 uint8_t *mrbuf, uint32_t *mrlen)
{
    uint64_t va_pages, commit_pages, flags, size_b, cookie = 0, out_flags;
    uint64_t cookie_pg, free_va = 0;
    int64_t slice = -1;
    void *pages, *gva = MAP_FAILED;
    int idx, host_prot;
    bool imported = false;
    TeboxMaliMap *mp;
    struct tbxm_base_mem_import_user_buffer ub;
    union tbxm_kbase_ioctl_mem_import u;
    struct tebox_mali_mmap_resp *mr;

    *mrlen = 0;
    if (!tbxm_env_win_import() || size < 32) {
        return 1;
    }
    memcpy(&va_pages, buf, 8);
    memcpy(&commit_pages, buf + 8, 8);
    memcpy(&flags, buf + 24, 8);
    /* Fully committed, CPU-visible, >= 4 MiB. Smaller allocs stay native so
     * boot does not spend the 64 kbase cookies on UI buffers. GROW_ON_GPF
     * is allowed when the region is already fully committed. */
    if (!va_pages || va_pages >= (1ull << 28) || va_pages < 1024 ||
        commit_pages != va_pages) {
        tbxm_win_skip(buf, size, "shape");
        return 1;
    }
    if (!(flags & (TBXM_BASE_MEM_PROT_CPU_RD | TBXM_BASE_MEM_PROT_CPU_WR)) ||
        (!(flags & TBXM_BASE_MEM_SAME_VA) &&
         !(flags & TBXM_BASE_MEM_NEED_MMAP))) {
        tbxm_win_skip(buf, size, "flags");
        return 1;
    }
    size_b = va_pages << 12;
    if (!size_b || size_b > TBXM_WIN_BYTES) {
        tbxm_win_skip(buf, size, "size");
        return 1;
    }
    if (tbxm_win_ensure(m) < 0) {
        tbxm_win_fail++;
        return 1;
    }
    slice = tbxm_win_slice_alloc(m, size_b);
    if (slice < 0) {
        tbxm_win_skip(buf, size, "full");
        tbxm_win_fail++;
        return 1;
    }
    pages = (uint8_t *)m->win_base + slice;
    memset(pages, 0, (size_t)size_b);

    memset(&u, 0, sizeof(u));
    ub.ptr = (uint64_t)(uintptr_t)pages;
    ub.length = size_b;
    /*
     * kbase rejects MEM_ALLOC flags on import ("bad flags" → -ENOMEM).
     * USER_BUFFER input is CPU/GPU prot + cache/coherent only; SAME_VA and
     * NEED_MMAP are applied by kbase itself.
     */
    u.in.flags = (flags & (TBXM_BASE_MEM_PROT_CPU_RD | TBXM_BASE_MEM_PROT_CPU_WR |
                           (1ull << 2) | (1ull << 3))) |
                 TBXM_BASE_MEM_CACHED_CPU | TBXM_BASE_MEM_COHERENT_SYSTEM;
    u.in.phandle = (uint64_t)(uintptr_t)&ub;
    u.in.type = TBXM_BASE_MEM_IMPORT_TYPE_USER_BUFFER;
    if (tbxm_ioctl(c->fd, TBXM_KBASE_IOCTL_MEM_IMPORT, &u)) {
        tbxm_warn_rl("ctx %u win MEM_IMPORT len %" PRIu64 " -> %s",
                     ctx, size_b, strerror(errno));
        goto fail;
    }
    imported = true;
    cookie = u.out.gpu_va;
    free_va = cookie;
    cookie_pg = cookie >> 12;
    /*
     * munmap must match kbase's VMA exactly. A larger length unmaps the
     * neighbouring region ("Enclosing region not found") and wedges CSF.
     * A non-cookie gpu_va means this kbase already realized the VA; libmali
     * would treat our reply as a cookie and fault at that address.
     */
    if (!u.out.va_pages || u.out.va_pages != va_pages ||
        cookie_pg < TBXM_COOKIE_FIRST ||
        cookie_pg >= TBXM_COOKIE_FIRST + TBXM_NR_COOKIES) {
        tbxm_warn_rl("ctx %u win import va=%" PRIu64 " out_va=%" PRIu64
                     " gpu_va=0x%" PRIx64, ctx, va_pages, u.out.va_pages,
                     cookie);
        goto fail;
    }
    host_prot = PROT_READ | PROT_WRITE;
    if ((flags & TBXM_BASE_MEM_PROT_CPU_RD) &&
        !(flags & TBXM_BASE_MEM_PROT_CPU_WR)) {
        host_prot = PROT_READ;
    }
    gva = mmap(NULL, (size_t)size_b, host_prot, MAP_SHARED, c->fd,
               (off_t)cookie);
    if (gva == MAP_FAILED) {
        gva = mmap(NULL, (size_t)size_b, PROT_READ | PROT_WRITE, MAP_SHARED,
                   c->fd, (off_t)cookie);
        host_prot = PROT_READ | PROT_WRITE;
    }
    if (gva == MAP_FAILED) {
        tbxm_warn_rl("ctx %u win mmap(cookie 0x%" PRIx64 ") -> %s",
                     ctx, cookie, strerror(errno));
        goto fail;
    }
    /*
     * If kbase pinned the GPU VA at the window HVA, munmap(gva) would punch
     * the one KVM slot. Drop the import and use native MEM_ALLOC instead.
     */
    if ((uintptr_t)gva >= (uintptr_t)m->win_base &&
        (uintptr_t)gva < (uintptr_t)m->win_base + m->win_size) {
        tbxm_warn_rl("ctx %u win GPU VA %p glued to window", ctx, gva);
        free_va = (uint64_t)(uintptr_t)gva;
        gva = MAP_FAILED;
        goto fail;
    }
    free_va = (uint64_t)(uintptr_t)gva;

    idx = tbxm_map_slot_alloc(m);
    if (idx < 0) {
        goto fail;
    }
    mp = &m->map[idx];
    memset(mp, 0, sizeof(*mp));
    mp->used = true;
    mp->win_import = true;
    mp->ctx = (int)ctx;
    mp->host_ptr = gva;
    mp->pgoff = cookie_pg;
    mp->size = size_b;
    mp->hm_off = m->win_hm_off + (uint64_t)slice;
    mp->host_prot = host_prot;
    mp->buf = NULL;
    gva = MAP_FAILED; /* map slot owns the cookie VMA */

    /*
     * Force NEED_MMAP. Last time out.flags came back without it and libmali
     * used the cookie (0x41000) as the GPU VA — the shader then page-faulted
     * there. The eager mmap_resp holds the realized VA.
     */
    out_flags = (u.out.flags ? u.out.flags : flags) |
                TBXM_BASE_MEM_SAME_VA | TBXM_BASE_MEM_NEED_MMAP;
    out_flags &= ~TBXM_BASE_MEM_GROW_ON_GPF;
    memcpy(buf, &out_flags, 8);
    memcpy(buf + 8, &cookie, 8);

    mr = (struct tebox_mali_mmap_resp *)mrbuf;
    memset(mr, 0, sizeof(*mr));
    mr->hostmem_offset = mp->hm_off;
    mr->size = size_b;
    mr->gpu_va = (uint64_t)(uintptr_t)mp->host_ptr;
    mr->host_prot = (uint32_t)host_prot;
    mr->flags = TEBOX_MALI_MMAP_F_PROT;
    mr->handle = (uint32_t)(idx + 1);
    *mrlen = sizeof(*mr);
    tbxm_win_ok++;
    if (tbxm_win_ok <= 4 || (tbxm_win_ok % 64) == 0) {
        info_report("tebox-mali: win-import ok=%" PRIu64 " fail=%" PRIu64
                    " pages=%" PRIu64 " cookie=0x%" PRIx64
                    " gpu_va=%p hm_off=0x%" PRIx64 " out_flags=0x%" PRIx64,
                    tbxm_win_ok, tbxm_win_fail, va_pages, cookie,
                    mp->host_ptr, mp->hm_off, out_flags);
    }
    return 0;

fail:
    if (gva != MAP_FAILED) {
        munmap(gva, (size_t)size_b);
    } else if (imported) {
        tbxm_ioctl(c->fd, _IOW(TBXM_KBASE_IOCTL_TYPE, TBXM_KBASE_NR_MEM_FREE,
                               uint64_t), &free_va);
    }
    if (slice >= 0 && m->win_used) {
        bitmap_clear(m->win_used, (uint64_t)slice / TEBOX_MALI_PAGE,
                     size_b / TEBOX_MALI_PAGE);
    }
    tbxm_win_fail++;
    return 1;
}

/* the headers; the (possibly modified) struct is written back to in_sg. */
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
    uint64_t alloc_va_pages = 0;
    bool is_bind = ((req->cmd >> 8) & 0xff) == TBXM_KBASE_IOCTL_TYPE &&
                   (req->cmd & 0xff) == TBXM_KBASE_NR_QUEUE_BIND && size >= 16;
    bool is_alloc = ((req->cmd >> 8) & 0xff) == TBXM_KBASE_IOCTL_TYPE &&
                    ((req->cmd & 0xff) == TBXM_KBASE_NR_MEM_ALLOC ||
                     (req->cmd & 0xff) == TBXM_KBASE_NR_MEM_ALLOC_EX) &&
                    size >= 16;
    if (is_bind) {
        memcpy(&bind_ring, buf, 8);
    }
    /* MEM_ALLOC overlays out on in; va_pages is only valid before the ioctl. */
    if (is_alloc) {
        memcpy(&alloc_va_pages, buf, 8);
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
        /* Pending WIN_IMPORT cookie freed before mmap — reclaim window pages. */
        {
            uint64_t pg = gva >> 12;
            if (!(gva & 0xfff) && pg >= TBXM_COOKIE_FIRST &&
                pg < TBXM_COOKIE_FIRST + TBXM_NR_COOKIES) {
                tbxm_win_release_pending(m, c, (int)(pg - TBXM_COOKIE_FIRST));
            }
        }
        /*
         * Do NOT QUEUE_TERMINATE from MEM_FREE. Libmali orders TERMINATE
         * itself; injecting it here raced with in-flight CSF work and
         * produced host page-fault@0 / CS_RESOURCE_TERMINATED / "queue not
         * bound", with AnTuTu scores collapsing ~90%. Ring NO_USER_FREE is
         * cleared on guest QUEUE_TERMINATE or ctx close (tbxm_csf_terminate).
         */
    }

    /*
     * Window USER_BUFFER fast path: replace MEM_ALLOC+OP_MMAP with one import
     * that pins pages already covered by hostmem_background (no KVM churn).
     */
    if (is_alloc && alloc_va_pages && n_ind == 0) {
        uint8_t mrbuf[sizeof(struct tebox_mali_mmap_resp)];
        uint32_t mlen = 0;
        int wr = tbxm_win_import_alloc(m, c, ctx, buf, size, mrbuf, &mlen);

        if (wr == 0) {
            size_t roff = sizeof(struct tebox_mali_cmd_resp);

            if (size) {
                iov_from_buf(elem->in_sg, elem->in_num, roff, buf, size);
                roff += size;
            }
            if (mlen) {
                roff += iov_from_buf(elem->in_sg, elem->in_num, roff,
                                     mrbuf, mlen);
            }
            *out_len = (uint32_t)(roff - sizeof(struct tebox_mali_cmd_resp));
            ret = 0;
            goto out_free;
        }
    }

    bool prof_alloc = is_alloc && alloc_va_pages >= 1024;
    uint64_t prof_t0 = prof_alloc ? tbxm_prof_now() : 0;
    int r = tbxm_ioctl(c->fd, req->cmd, size ? buf : NULL);
    if (prof_alloc) {
        tbxm_prof.n_alloc++;
        tbxm_prof.alloc_ns += tbxm_prof_now() - prof_t0;
    }
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
     * [struct(size)][OUT buffers...][optional eager mmap_resp]. The struct is
     * shipped back as the kernel left it: for _IOW ioctls (GET_GPUPROPS) the
     * guest doesn't read it back, and for union _IOWR ioctls (GLB_IFACE) the
     * output overlays the pointer fields, so the host addresses are already
     * overwritten. */
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

    /*
     * Eager SAME_VA mmap: fold OP_MMAP into this MEM_ALLOC virtio round-trip
     * so get_unmapped_area is a guest-side cache hit (no second kick).
     */
    if (is_alloc && alloc_va_pages && alloc_va_pages < (1ull << 28)) {
        uint64_t flags, gpu_va, pg;
        uint8_t mrbuf[sizeof(struct tebox_mali_mmap_resp)];
        uint32_t mlen = 0;
        struct tebox_mali_mmap_req mreq;

        memcpy(&flags, buf, 8);
        memcpy(&gpu_va, buf + 8, 8);
        pg = gpu_va >> 12;
        if (!(gpu_va & 0xfff) && pg >= TBXM_COOKIE_FIRST &&
            pg < TBXM_COOKIE_FIRST + TBXM_NR_COOKIES) {
            mreq.pgoff = pg;
            mreq.size = alloc_va_pages << 12;
            if (tbxm_op_mmap(m, ctx, &mreq, mrbuf, &mlen) == 0 &&
                mlen == sizeof(struct tebox_mali_mmap_resp)) {
                roff += iov_from_buf(elem->in_sg, elem->in_num, roff,
                                     mrbuf, mlen);
            }
        }
        (void)flags;
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
 * Build the prot try list for a kbase mmap. GPU-only (PROT_NONE) is tried
 * alone; otherwise want first, then the R/W → R → NONE ladder.
 */
static int tbxm_kbase_prot_tries(int want, int *tries, int max)
{
    static const int ladder[] = { PROT_READ | PROT_WRITE, PROT_READ, PROT_NONE };
    int n = 0;

    if (want == PROT_NONE) {
        tries[n++] = PROT_NONE;
    } else if (want >= 0) {
        tries[n++] = want;
        for (size_t i = 0; i < ARRAY_SIZE(ladder) && n < max; i++) {
            if (ladder[i] != want) {
                tries[n++] = ladder[i];
            }
        }
    } else {
        for (size_t i = 0; i < ARRAY_SIZE(ladder) && n < max; i++) {
            tries[n++] = ladder[i];
        }
    }
    return n;
}

/*
 * mmap |off| of the kbase fd. If |fixed| is non-NULL, prefer MAP_FIXED into
 * that address (VirGL hostmem window); on failure try mremap(MREMAP_FIXED),
 * then fall back to an any-address mmap (caller may wrap as slow MR).
 */
static void *tbxm_kbase_mmap(int fd, off_t off, size_t size, int want,
                             void *fixed, int *prot_out, bool *used_fixed)
{
    int tries[4], n, e = EINVAL;

    *used_fixed = false;
    n = tbxm_kbase_prot_tries(want, tries, ARRAY_SIZE(tries));

    if (fixed) {
        /*
         * Do NOT munmap(|fixed|) before MAP_FIXED: the pooled/background MR
         * is still in KVM. A hole under a live memslot → kvm run Bad address
         * (broke boot). MAP_FIXED must replace the anon placeholder in place.
         */
        for (int i = 0; i < n; i++) {
            void *p = mmap(fixed, size, tries[i],
                           MAP_SHARED | MAP_FIXED, fd, off);
            if (p != MAP_FAILED) {
                *prot_out = tries[i];
                *used_fixed = true;
                tbxm_fixed_ok++;
                return p;
            }
            e = errno;
            if (e != EPERM && e != EACCES && e != EINVAL) {
                break;
            }
        }
        /* MAP_FIXED rejected: place anywhere then try to slide into |fixed|. */
        for (int i = 0; i < n; i++) {
            void *p = mmap(NULL, size, tries[i], MAP_SHARED, fd, off);
            if (p == MAP_FAILED) {
                e = errno;
                if (e != EPERM && e != EACCES && e != EINVAL) {
                    break;
                }
                continue;
            }
            void *q = mremap(p, size, size,
                             MREMAP_MAYMOVE | MREMAP_FIXED, fixed);
            if (q != MAP_FAILED) {
                *prot_out = tries[i];
                *used_fixed = true;
                tbxm_fixed_ok++;
                return q;
            }
            /*
             * mremap failed: keep the any-address mapping. Munmapping here
             * would drop the kbase cookie realization and make a retry
             * return EINVAL (broke EGL after buf-pool recycle).
             * |fixed| still holds the anon placeholder under the KVM slot.
             */
            e = errno;
            tbxm_fixed_miss++;
            if (!tbxm_fixed_miss_errno) {
                tbxm_fixed_miss_errno = e;
                warn_report("tebox-mali: MAP_FIXED/mremap miss size=%zu "
                            "prot=%d errno=%d (%s) — slow MR path",
                            size, tries[i], e, strerror(e));
            }
            *prot_out = tries[i];
            *used_fixed = false;
            return p;
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

static bool tbxm_mr_is_zone(TeboxMali *m, MemoryRegion *mr)
{
    if (m->arena_ready && mr == m->arena_mr) {
        return true;
    }
    for (int i = 0; i < TBXM_ZONES; i++) {
        if (m->zone[i].used && m->zone[i].mr && mr == m->zone[i].mr) {
            return true;
        }
    }
    return false;
}

/* End of the VMA that starts at addr, or addr+fallback if maps can't say. */
static uintptr_t tbxm_host_vma_end(void *addr, uint64_t fallback)
{
    FILE *fp;
    char line[512];
    uintptr_t want = (uintptr_t)addr;
    uintptr_t start, end;

    fp = fopen("/proc/self/maps", "r");
    if (!fp) {
        return want + fallback;
    }
    while (fgets(line, sizeof(line), fp)) {
        if (sscanf(line, "%" SCNxPTR "-%" SCNxPTR, &start, &end) == 2 &&
            start == want && end > want) {
            fclose(fp);
            return end;
        }
    }
    fclose(fp);
    return want + fallback;
}

/* Growable-zone helpers (init_ram_ptr_resizeable) are unfinished; arena/window
 * cover the boot path. Zone slots still tear down in realize/unrealize. */

#ifndef MAP_FIXED_NOREPLACE
#define MAP_FIXED_NOREPLACE 0x100000
#endif

/* One slot over [low, low+8GiB). The first VMA sits near the top. */
static bool tbxm_arena_create(TeboxMali *m, void *p, uint64_t cover)
{
    uintptr_t pp = (uintptr_t)p;
    uintptr_t low, span;
    void *res;
    int64_t off;
    MemoryRegion *mr;
    g_autofree char *name = g_strdup("tebox-mali-arena");

    if (cover + TBXM_ARENA_HEAD >= TBXM_ARENA_BYTES) {
        return false;
    }
    span = TBXM_ARENA_BYTES - TBXM_ARENA_HEAD - cover;
    if (pp < span + 0x10000) {
        return false;
    }
    low = pp - span;
    if (low < 0x10000) {
        return false;
    }
    res = mmap((void *)low, span, PROT_NONE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (res == MAP_FAILED) {
        tbxm_warn_rl("arena reserve %p+0x%" PRIx64 " failed: %s",
                     (void *)low, (uint64_t)span, strerror(errno));
        return false;
    }
    off = tbxm_hm_alloc(m, TBXM_ARENA_BYTES);
    if (off < 0) {
        munmap(res, span);
        tbxm_warn_rl("arena BAR reserve 8GiB failed");
        return false;
    }
    mr = g_new0(MemoryRegion, 1);
    memory_region_init_ram_ptr(mr, OBJECT(m), name, TBXM_ARENA_BYTES,
                               (void *)low);
    memory_region_add_subregion(&m->hostmem, (uint64_t)off, mr);
    /* Slot is registered. Drop the placeholder so kbase can map below. */
    munmap((void *)low, span);
    m->arena_mr = mr;
    m->arena_base = (void *)low;
    m->arena_size = TBXM_ARENA_BYTES;
    m->arena_hm = (uint64_t)off;
    m->arena_ready = true;
    tbxm_prof.n_zone_new++;
    info_report("tebox-mali: arena base=%p size=0x%" PRIx64
                " first=%p cover=0x%" PRIx64 " hm=0x%" PRIx64,
                (void *)low, (uint64_t)TBXM_ARENA_BYTES, p, cover,
                (uint64_t)off);
    return true;
}

/*
 * GPA of a mali VMA inside the one pre-registered slot, or UINT64_MAX to
 * keep today's per-buffer MR. *cover_out is the VMA length.
 */
static uint64_t tbxm_zone_place(TeboxMali *m, void *p, uint64_t size,
                                int host_prot, uint64_t *cover_out)
{
    uintptr_t pp, end;
    uint64_t cover;

    (void)host_prot;
    if (!p || p == MAP_FAILED || !size) {
        return UINT64_MAX;
    }
    pp = (uintptr_t)p;
    end = size >= (4ull << 20) ? tbxm_host_vma_end(p, size) : pp + size;
    if (end < pp + size) {
        end = pp + size;
    }
    cover = (uint64_t)(end - pp);
    *cover_out = cover;

    if (m->arena_ready) {
        uintptr_t base = (uintptr_t)m->arena_base;

        if (pp >= base && end <= base + m->arena_size) {
            tbxm_prof.n_grow++;
            return m->arena_hm + (uint64_t)(pp - base);
        }
        if (size >= (4ull << 20)) {
            tbxm_prof.n_zone_skip++;
        }
        return UINT64_MAX;
    }
    if (size < (4ull << 20)) {
        return UINT64_MAX;
    }
    if (!tbxm_arena_create(m, p, cover)) {
        tbxm_prof.n_zone_skip++;
        return UINT64_MAX;
    }
    return m->arena_hm + (uint64_t)(pp - (uintptr_t)m->arena_base);
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
    int idx = tbxm_map_slot_alloc(m);
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

    /*
     * Prefer a recycled BAR buf: MAP_FIXED mali into its HVA (zero KVM churn).
     * CIX kbase currently returns EINVAL for every MAP_FIXED/mremap-FIXED
     * (see fixed_miss in /tmp/tbxmali.log) — warm alloc falls back to rebind.
     */
    TeboxMaliBuf *pooled = NULL;
    void *fixed = NULL;
    int64_t hm_off = -1;
    if (want != PROT_NONE) {
        /*
         * Always recycle pooled BAR bufs. With MAP_FIXED off (CIX default),
         * pop still matters: free only munmaps+parks the MR; the next alloc
         * steals the GPA and rebinds (del deferred off the free path).
         * Skipping pop filled the pool and made free fall back to del_subregion.
         */
        pooled = tbxm_buf_pool_pop(m, size);
        if (pooled) {
            hm_off = (int64_t)pooled->hm_off;
            if (tbxm_env_map_fixed()) {
                fixed = pooled->host_ptr;
            }
        } else {
            hm_off = tbxm_hm_alloc(m, size);
            if (hm_off < 0) {
                tbxm_warn_rl("out of hostmem window (ctx %u, need %" PRIu64
                             " bytes of %" PRIu64 ")", ctx, size,
                             m->hostmem_size);
                tbxm_map_slot_free(m, idx);
                return -ENOMEM;
            }
            /* CIX kbase rejects MAP_FIXED of mali0; only probe when env set. */
            if (m->hostmem_mmap && tbxm_env_map_fixed()) {
                fixed = (uint8_t *)m->hostmem_mmap + hm_off;
            }
        }
    }
    int host_prot = PROT_NONE;
    bool used_fixed = false;
    bool prof_map = size >= (4ull << 20);
    uint64_t prof_map_ns = 0, prof_t0 = prof_map ? tbxm_prof_now() : 0;
    void *p = tbxm_kbase_mmap(c->fd, (off_t)(pg << 12), (size_t)size, want,
                              fixed, &host_prot, &used_fixed);
    if (prof_map) {
        prof_map_ns = tbxm_prof_now() - prof_t0;
    }
    if (p == MAP_FAILED || host_prot == PROT_NONE) {
        if (hm_off >= 0) {
            if (used_fixed && p != MAP_FAILED) {
                munmap(p, (size_t)size);
                tbxm_hm_restore_anon(fixed, (size_t)size);
                p = mmap(NULL, (size_t)size, PROT_NONE, MAP_SHARED, c->fd,
                         (off_t)(pg << 12));
                used_fixed = false;
                if (p == MAP_FAILED) {
                    host_prot = PROT_NONE;
                }
            } else if (fixed && !used_fixed) {
                tbxm_hm_restore_anon(fixed, (size_t)size);
            }
            if (pooled) {
                /* GPA still owned by the pooled MR — put it back. */
                if (!tbxm_buf_pool_push(m, pooled)) {
                    bitmap_set(m->hm_used, pooled->hm_off / TEBOX_MALI_PAGE,
                               pooled->size / TEBOX_MALI_PAGE);
                    memory_region_del_subregion(&m->hostmem, &pooled->mr);
                    object_unparent(OBJECT(pooled));
                }
                pooled = NULL;
            } else if (tbxm_fl_class(size) >= 0) {
                tbxm_fl_push(m, (uint64_t)hm_off, size);
            } else {
                bitmap_clear(m->hm_used, hm_off / TEBOX_MALI_PAGE,
                             size / TEBOX_MALI_PAGE);
            }
            hm_off = -1;
            fixed = NULL;
        }
    }
    if (p == MAP_FAILED) {
        int e = errno;
        tbxm_map_slot_free(m, idx);
        tbxm_warn_rl("ctx %u mmap pgoff=0x%" PRIx64 " size=%" PRIu64
                     " want=%d failed: %s", ctx, pg, size, want, strerror(e));
        return -e;
    }
    if (cookie) {
        *cookie = 0;    /* consumed: kbase recycles the cookie */
    }
    if (prof_map) {
        prof_t0 = tbxm_prof_now();
    }

    TeboxMaliMap *mp = &m->map[idx];
    mp->used = true;
    mp->map_fixed = false;
    mp->zone = false;
    mp->ctx = (int)ctx;
    mp->host_ptr = p;
    mp->pgoff = pg;
    mp->size = size;
    mp->host_prot = host_prot;
    mp->hm_off = 0;
    mp->buf = NULL;

    if (!used_fixed) {
        uint64_t cover = size;
        uint64_t zoff = tbxm_zone_place(m, p, size, host_prot, &cover);

        if (zoff != UINT64_MAX) {
            mp->zone = true;
            mp->hm_off = zoff;
            mp->size = cover;
            if (pooled) {
                if (!tbxm_buf_pool_push(m, pooled)) {
                    memory_region_del_subregion(&m->hostmem, &pooled->mr);
                    object_unparent(OBJECT(pooled));
                }
                pooled = NULL;
            }
        }
    }

    if (mp->zone) {
        /* Slot already covers this VMA. */
    } else if (pooled && used_fixed) {
        /* Recycled BAR slot: KVM memslot untouched. */
        pooled->host_ptr = p;
        if (!(host_prot & PROT_WRITE)) {
            memory_region_set_readonly(&pooled->mr, true);
        } else {
            memory_region_set_readonly(&pooled->mr, false);
        }
        mp->hm_off = pooled->hm_off;
        mp->buf = pooled;
        pooled = NULL;
    } else if (hm_off >= 0 && used_fixed && m->hostmem_mmap) {
        /* VirGL window path (opt-in TEBOX_MALI_MAP_FIXED=1). */
        mp->map_fixed = true;
        mp->hm_off = (uint64_t)hm_off;
        if (pooled) {
            if (!tbxm_buf_pool_push(m, pooled)) {
                memory_region_del_subregion(&m->hostmem, &pooled->mr);
                object_unparent(OBJECT(pooled));
            }
            pooled = NULL;
        }
    } else if (hm_off >= 0) {
        TeboxMaliBuf *b;

        if (pooled) {
            /*
             * MAP_FIXED into the recycled HVA failed; mali landed at a new VA.
             * Rebind at the same GPA: one del+add (same as cold) but do NOT
             * leave a dead pooled MR parked forever (that ballooned KVM
             * slots and tanked fps/free after pool-keep).
             */
            hm_off = (int64_t)tbxm_buf_steal_gpa(m, pooled);
            pooled = NULL;
        }
        /* Cold / rebind path: per-buffer MR (one KVM slot add). */
        {
            static uint64_t buf_seq;
            g_autofree char *name =
                g_strdup_printf("tm-buf-%" PRIu64, ++buf_seq);

            b = g_new0(TeboxMaliBuf, 1);
            object_initialize_child(OBJECT(m), name, b, TYPE_TEBOX_MALI_BUF);
            b->m = m;
            b->hm_off = (uint64_t)hm_off;
            b->size = size;
            b->host_ptr = p;
            /* Device for USER_IO + tiny CQS/sync only (default 64KiB). */
            {
                uint64_t dev_max = 64ull * 1024;
                const char *e = getenv("TEBOX_MALI_DEVICE_MAX");
                bool user_io = tbxm_is_user_io(mp);

                if (e && *e) {
                    dev_max = strtoull(e, NULL, 0);
                }
                if (user_io || size <= dev_max) {
                    memory_region_init_ram_device_ptr(&b->mr, OBJECT(b), "mr",
                                                     size, p);
                } else {
                    memory_region_init_ram_ptr(&b->mr, OBJECT(b), "mr", size, p);
                }
            }
            if (!(host_prot & PROT_WRITE)) {
                memory_region_set_readonly(&b->mr, true);
            }
            if (tbxm_hm_subregion_overlap(m, (uint64_t)hm_off, size)) {
                tbxm_hm_evict_overlap(m, (uint64_t)hm_off, size);
            }
            if (m->hostmem_mmap) {
                memory_region_add_subregion_overlap(&m->hostmem, b->hm_off,
                                                   &b->mr, 1);
            } else {
                memory_region_add_subregion(&m->hostmem, b->hm_off, &b->mr);
            }
            mp->hm_off = b->hm_off;
            mp->buf = b;
        }
    } else if (pooled) {
        if (!tbxm_buf_pool_push(m, pooled)) {
            memory_region_del_subregion(&m->hostmem, &pooled->mr);
            object_unparent(OBJECT(pooled));
        }
    }

    if (prof_map) {
        tbxm_prof.n_mmap++;
        tbxm_prof.mmap_ns += prof_map_ns;
        tbxm_prof.slot_ns += tbxm_prof_now() - prof_t0;
        if (tbxm_prof.n_mmap == 1 || (tbxm_prof.n_mmap % 16) == 0) {
            tbxm_prof_report();
        }
    }
    if (tbxm_trace()) {
        info_report("tebox-mali: ctx %u mmap pgoff=0x%" PRIx64 " size=%" PRIu64
                    " want=%d -> host %p prot=%d hm_off=0x%" PRIx64
                    " fixed=%d handle=%d", ctx, pg, size, want, p, host_prot,
                    mp->hm_off, mp->map_fixed, idx + 1);
    }

    struct tebox_mali_mmap_resp *r = out;
    memset(r, 0, sizeof(*r));
    r->hostmem_offset = (mp->hm_off == UINT64_MAX) ? 0 : mp->hm_off;
    r->size = size;
    r->gpu_va = (uint64_t)(uintptr_t)p;   /* host map addr == GPU VA (SAME_VA) */
    r->host_prot = (uint32_t)host_prot;
    r->flags = TEBOX_MALI_MMAP_F_PROT;
    r->handle = (uint32_t)(idx + 1);
    *out_len = sizeof(*r);
    /* Cookie-range pgoff with a cookie-like gpu_va means SAME_VA broke. */
    if (pg >= TBXM_COOKIE_FIRST && pg < TBXM_COOKIE_FIRST + TBXM_NR_COOKIES &&
        r->gpu_va < 0x100000000ULL) {
        tbxm_warn_rl("ctx %u BAD SAME_VA pgoff=0x%" PRIx64 " gpu_va=0x%" PRIx64
                     " size=%" PRIu64,
                     ctx, pg, r->gpu_va, size);
    }
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
    /* Coalesce consecutive OP_MMAP/OP_MUNMAP* into one FlatView/KVM update.
     * Commit before any other op (ioctl may touch the BAR) and before notify. */
    bool mr_txn = false;

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
        bool mr_op;

        if (iov_to_buf(elem->out_sg, elem->out_num, 0, &hdr, sizeof(hdr)) !=
            sizeof(hdr)) {
            resp.result = -EINVAL;
            goto reply;
        }
        resp.seq = hdr.seq;

        mr_op = (hdr.op == TEBOX_MALI_OP_MMAP ||
                 hdr.op == TEBOX_MALI_OP_MUNMAP);
        if (mr_op && !mr_txn) {
            memory_region_transaction_begin();
            mr_txn = true;
        } else if (!mr_op && mr_txn) {
            memory_region_transaction_commit();
            mr_txn = false;
        }

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
                            "read=%u open=%u close=%u top_nr=%u:%u "
                            "fixed_ok=%" PRIu64 " fixed_miss=%" PRIu64
                            " miss_errno=%d wok=%" PRIu64 " wfail=%" PRIu64
                            "\n",
                            cnt[TEBOX_MALI_OP_IOCTL], cnt[TEBOX_MALI_OP_MMAP],
                            cnt[TEBOX_MALI_OP_MUNMAP], cnt[TEBOX_MALI_OP_IMPORT],
                            cnt[TEBOX_MALI_OP_READ], cnt[TEBOX_MALI_OP_OPEN],
                            cnt[TEBOX_MALI_OP_CLOSE], top_nr, top_n,
                            tbxm_fixed_ok, tbxm_fixed_miss,
                            tbxm_fixed_miss_errno, tbxm_win_ok, tbxm_win_fail);
                    fclose(f);
                }
                memset(cnt, 0, sizeof(cnt));
                memset(nr_cnt, 0, sizeof(nr_cnt));
                tbxm_win_ok = tbxm_win_fail = 0;
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
    if (mr_txn) {
        memory_region_transaction_commit();
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
    /* hostmem BAR + hostmem_background are created by the PCI proxy realize
     * (before this), so virtio modern MMIO can leave BAR 4 first. */
    for (int i = 0; i < TEBOX_MALI_MAX_CTX; i++) {
        m->ctx[i].fd = -1;
        m->ctx[i].rearm = timer_new_ms(QEMU_CLOCK_REALTIME, tbxm_ctx_rearm,
                                       &m->ctx[i]);
    }
    m->win_mfd = -1;
    info_report("tebox-mali: virtio device ready (id=%d, hostmem=%" PRIu64
                " MiB, map_fixed=%s)",
                TEBOX_MALI_VIRTIO_ID, m->hostmem_size >> 20,
                m->hostmem_mmap ? "on" : "OFF");
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
    for (int i = 0; i < TBXM_FL_CLASSES; i++) {
        g_free(m->freelist[i].offs);
        m->freelist[i].offs = NULL;
        m->freelist[i].n = m->freelist[i].cap = 0;
    }
    g_free(m->hm_used);
    m->hm_used = NULL;
    if (m->hostmem_mmap) {
        memory_region_del_subregion(&m->hostmem, &m->hostmem_background);
        qemu_ram_munmap(-1, m->hostmem_mmap, m->hostmem_size);
        m->hostmem_mmap = NULL;
    }
    for (int i = 0; i < TBXM_ZONES; i++) {
        TeboxMaliZone *z = &m->zone[i];

        if (!z->used) {
            continue;
        }
        memory_region_del_subregion(&m->hostmem, z->mr);
        object_unparent(OBJECT(z->mr));
        g_free(z->mr);
        z->mr = NULL;
        if (z->base && z->base != MAP_FAILED && z->size) {
            munmap(z->base, z->size);
        }
        z->used = false;
    }
    if (m->arena_ready && m->arena_mr) {
        memory_region_del_subregion(&m->hostmem, m->arena_mr);
        object_unparent(OBJECT(m->arena_mr));
        g_free(m->arena_mr);
        m->arena_mr = NULL;
        if (m->arena_base && m->arena_size) {
            munmap(m->arena_base, m->arena_size);
        }
        m->arena_ready = false;
    }
    if (m->win_ready) {
        memory_region_del_subregion(&m->hostmem, &m->win_mr);
        if (m->win_base && m->win_base != MAP_FAILED) {
            munmap(m->win_base, m->win_size);
        }
        if (m->win_mfd >= 0) {
            close(m->win_mfd);
        }
        g_free(m->win_used);
        m->win_base = NULL;
        m->win_used = NULL;
        m->win_mfd = -1;
        m->win_ready = false;
    }
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

    /*
     * hostmem_background only for TEBOX_MALI_MAP_FIXED probe. Win-import no
     * longer uses a whole-BAR background (that made dmabuf import ENOMEM);
     * it recycles per-buffer MRs via USER_BUFFER instead.
     */
    memory_region_init(&dev->vdev.hostmem, OBJECT(&dev->vdev),
                       "tebox-mali-hostmem", sz);

    if (tbxm_env_map_fixed()) {
        void *map = qemu_ram_mmap(-1, sz, qemu_real_host_page_size(),
                                  QEMU_MAP_NORESERVE, 0);
        if (map == MAP_FAILED) {
            map = qemu_ram_mmap(-1, sz, qemu_real_host_page_size(), 0, 0);
        }
        if (map == MAP_FAILED) {
            error_setg_errno(errp, errno,
                             "tebox-mali hostmem_background mmap failed");
            return;
        }
        dev->vdev.hostmem_mmap = map;
        memory_region_init_ram_ptr(&dev->vdev.hostmem_background,
                                   OBJECT(&dev->vdev),
                                   "tebox-mali-hostmem-bg", sz, map);
        memory_region_add_subregion(&dev->vdev.hostmem, 0,
                                   &dev->vdev.hostmem_background);
        info_report("tebox-mali: hostmem_background %" PRIu64
                    " MiB (MAP_FIXED probe)", sz >> 20);
    } else {
        dev->vdev.hostmem_mmap = NULL;
    }

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
