/*
 * tebox-mali (local, not for upstream): wire protocol shared by the QEMU device
 * (hw/display/tebox-mali.c) and the guest kernel shim (tebox_mali.ko), which
 * passes the host CIX Mali kbase device /dev/mali0 (Mali-G720, CSF, DDK r49p0)
 * through to a QEMU guest so the stock CIX Mali userspace (or open PanVK) drives
 * the real GPU with no gfxstream/virgl translation.
 *
 * Design (see thirdparty/vendor-drivers/cix/mali/PASSTHROUGH-DESIGN.md):
 *  - Control path: a thin virtio CMD queue carries a GENERIC kbase ioctl
 *    forward. kbase's CSF ioctls (VERSION_CHECK, SET_FLAGS, MEM_ALLOC,
 *    CS_QUEUE_*, ...) are flat, pointer-free structs, so the guest ships the
 *    ioctl request number + _IOC_SIZE payload bytes and the host replays them
 *    on its real /dev/mali0 fd. One guest open() == one host fd (per-context).
 *  - Memory path (zero-copy): kbase is SAME_VA (GPU VA == CPU VA) and its
 *    buffers / CS user-IO pages are plain CPU mmap()s of the device fd. On a
 *    guest mmap the host mmap()s its /dev/mali0 fd at the same cookie, wraps the
 *    host pages as a RAM MemoryRegion inside the device's hostmem PCI BAR, and
 *    returns the window offset; the guest shim remaps the user VMA onto that BAR
 *    page. KVM then maps guest-PA -> the very host pages the GPU DMAs, so the
 *    guest CPU and the GPU share one physical buffer at full speed. The guest
 *    shim's get_unmapped_area returns the cookie (= gpu_va) so the userspace
 *    pointer equals gpu_va, preserving SAME_VA across the VM boundary.
 *
 * Little-endian wire (guest and host both aarch64 LE). All structs packed.
 */
#ifndef TEBOX_MALI_PROTO_H
#define TEBOX_MALI_PROTO_H

#include <stdint.h>

/* Local, non-upstream virtio device id (61; 60 is tebox-v4l2). Must match the
 * guest shim and be registered in hw/virtio/virtio.c virtio_device_names[]. */
#define TEBOX_MALI_VIRTIO_ID 61

/* Virtqueues. CMD: synchronous request/response. EVT: the guest posts empty
 * device-writable tebox_mali_event buffers; the host fills one per event. */
enum {
    TEBOX_MALI_VQ_CMD = 0,
    TEBOX_MALI_VQ_EVT = 1,
    TEBOX_MALI_VQ_MAX = 2,
};

/* EVT_READABLE: the ctx's kbase fd became readable (poll() wakeup). The host
 * then stops watching that fd until the guest's next OP_READ, so a level-
 * triggered fd can't flood the queue.
 * EVT_FENCE: the host sync_file behind KCPU fence |id| signaled. */
enum {
    TEBOX_MALI_EVT_READABLE = 1,
    TEBOX_MALI_EVT_FENCE = 2,
};

/* One CSF notification is 16–32 bytes. Keep this modest: each EVT_READABLE is
 * one virtio interrupt; packing several records per EVT cuts follow-up OP_READ
 * (the other half of the swipe-time KVM-exit bill) without ballooning the
 * event ring. Must match guest tebox-mali-abi.h. */
#define TEBOX_MALI_EVT_DATA 64

struct tebox_mali_event {
    uint32_t type;      /* TEBOX_MALI_EVT_*                    */
    uint32_t ctx;       /* EVT_READABLE: the context            */
    uint64_t id;        /* EVT_FENCE: the fence id              */
    uint32_t data_len;  /* EVT_READABLE: bytes in data[]        */
    uint32_t reserved;
    uint8_t  data[TEBOX_MALI_EVT_DATA];
} __attribute__((packed));

/* CMD opcodes. */
enum {
    TEBOX_MALI_OP_OPEN = 1,    /* open host /dev/mali0 -> ctx id            */
    TEBOX_MALI_OP_CLOSE = 2,
    TEBOX_MALI_OP_IOCTL = 3,   /* generic kbase ioctl forward (flat struct) */
    TEBOX_MALI_OP_MMAP = 4,    /* map a kbase buffer into the hostmem BAR   */
    TEBOX_MALI_OP_MUNMAP = 5,
    TEBOX_MALI_OP_IMPORT = 6,  /* import a guest dmabuf's pages into kbase  */
    TEBOX_MALI_OP_READ = 7,    /* read() the kbase fd: GPU completion events */
};

/* OP_READ: the stock Mali userspace learns of GPU completion by poll()+read()ing
 * the kbase fd, which returns base_csf_notification event records. The host
 * does a NON-BLOCKING read on its fd (opened O_NONBLOCK so it never stalls the
 * QEMU event loop), ships the bytes back and re-arms EVT_READABLE; the guest
 * shim sleeps on that event to present blocking read()/poll() semantics.
 * Request: [cmd_hdr][read_req]. Response: [cmd_resp][raw bytes]; resp.result =
 * bytes or -errno (-EAGAIN when no event is pending). */
struct tebox_mali_read_req {
    uint32_t max_len;   /* bytes the guest's read buffer can hold */
    uint32_t reserved;
} __attribute__((packed));

/* OP_IMPORT: the stock Mali blob allocates its buffers from a guest dma-heap and
 * imports the dmabuf into kbase (UMM). The host can't see the guest dmabuf fd,
 * so the guest resolves it to its guest-physical page segments and sends them;
 * the host maps each segment to its own VA (guest RAM is host memory) and does a
 * kbase USER_BUFFER import, so the real GPU DMAs the guest buffer's pages in
 * place (zero copy). Request: [cmd_hdr][import_req][n_seg * import_seg].
 * Response payload: [cmd_resp][import_resp]. */
struct tebox_mali_import_req {
    uint64_t flags;     /* kbase BASE_MEM flags passed by the guest   */
    uint32_t type;      /* original base_mem_import_type (UMM)        */
    uint32_t n_seg;     /* guest-physical page segments that follow   */
} __attribute__((packed));

struct tebox_mali_import_seg {
    uint64_t gpa;       /* guest physical address of the segment      */
    uint64_t len;       /* bytes                                      */
} __attribute__((packed));

struct tebox_mali_import_resp {
    uint64_t gpu_va;
    uint64_t va_pages;
} __attribute__((packed));

/* Every request starts with this. |ctx| selects the per-open host fd. */
struct tebox_mali_cmd_hdr {
    uint32_t op;        /* TEBOX_MALI_OP_*                      */
    uint32_t ctx;       /* per-open context id (ignored for OPEN) */
    uint32_t seq;       /* request serial                        */
    uint32_t len;       /* bytes of op payload that follow       */
} __attribute__((packed));

/* Response header the host writes back. */
struct tebox_mali_cmd_resp {
    uint32_t seq;       /* echoes the request    */
    int32_t  result;    /* 0 or -errno           */
    uint32_t len;       /* bytes of payload that follow */
    uint32_t reserved;
} __attribute__((packed));

/* OP_OPEN response payload: the assigned context id. */
struct tebox_mali_open_resp {
    uint32_t ctx;
} __attribute__((packed));

/* OP_IOCTL request. Some kbase ioctls embed USER POINTERS in their flat struct
 * (GET_GPUPROPS.buffer, CS_GET_GLB_IFACE.groups_ptr/streams_ptr, MEM_IMPORT, ...)
 * which are guest VAs the host can't dereference. The guest therefore attaches
 * |n_ind| "indirect buffer" descriptors: for each, the host allocates |len|
 * bytes, copies IN data into it, patches the struct's pointer field at
 * |field_off| to the host buffer, runs the ioctl, and ships OUT buffers back.
 *
 * Wire (request):  [cmd_hdr][ioctl_req][struct bytes = size]
 *                  [n_ind * tebox_mali_ioctl_ind][concatenated IN buffers]
 * Wire (response): [cmd_resp][struct bytes = size][concatenated OUT buffers]
 * resp.result = the ioctl's own return value (>=0) or -errno. */
struct tebox_mali_ioctl_req {
    uint32_t cmd;       /* the ioctl request number (encodes dir + size) */
    uint32_t size;      /* _IOC_SIZE(cmd): bytes of struct that follow    */
    uint32_t n_ind;     /* number of indirect-pointer descriptors         */
    uint32_t reserved;
} __attribute__((packed));

/* IND_FENCE_OUT: the buffer is a struct base_fence that kbase fills with a new
 * host sync_file fd (KCPU FENCE_SIGNAL). The host keeps the fd and replaces
 * basep.fd with a fence id; EVT_FENCE reports that id once it signals. */
enum {
    TEBOX_MALI_IND_IN = 1,
    TEBOX_MALI_IND_OUT = 2,
    TEBOX_MALI_IND_FENCE_OUT = 4,
};

/* One embedded user pointer to relocate to a host buffer. |parent| 0: the
 * pointer field is in the ioctl struct; k > 0: it is inside the buffer of
 * descriptor k-1 (which must come earlier), e.g. KCPU_QUEUE_ENQUEUE's command
 * array whose commands point at CQS/fence/JIT arrays. */
struct tebox_mali_ioctl_ind {
    uint32_t field_off; /* byte offset of the u64 pointer field in the parent */
    uint32_t len;       /* bytes the host buffer must hold                    */
    uint32_t dir;       /* TEBOX_MALI_IND_*                                   */
    uint32_t parent;
} __attribute__((packed));

/* OP_MMAP request: map |size| bytes of the host fd at |pgoff| (the kbase mmap
 * cookie, e.g. out.gpu_va for a BO, or a special BASE_MEM handle). */
struct tebox_mali_mmap_req {
    uint64_t pgoff;     /* mmap offset >> PAGE_SHIFT (the cookie/handle)  */
    uint64_t size;      /* bytes, page-multiple                           */
} __attribute__((packed));

/* OP_MMAP response: where the host actually mapped the buffer and its window
 * offset. CRITICAL for SAME_VA: kbase assigns a buffer's GPU VA at mmap() time
 * to wherever the host maps it, so the host maps FIRST (here) and reports
 * |gpu_va| (= the host mapping address = the GPU VA); the guest then places its
 * own user mapping at that SAME address (via get_unmapped_area) so the guest
 * CPU pointer equals the GPU VA. |hostmem_offset| is where the pages live in the
 * hostmem BAR, which the guest remaps onto.
 *
 * |host_prot| is the PROT_* of the host mapping, taken from the region's
 * BASE_MEM_PROT_CPU_RD/WR exactly as native libmali would request it. The guest
 * must not map wider than that: a writable PTE over a read-only host page kills
 * the VM ("kvm run failed Bad address"). PROT_NONE (GPU-only memory) has no BAR
 * pages; hostmem_offset is then meaningless. */
struct tebox_mali_mmap_resp {
    uint64_t hostmem_offset;
    uint64_t size;
    uint64_t gpu_va;        /* host mapping address == the GPU VA (SAME_VA) */
    uint32_t host_prot;     /* valid iff TEBOX_MALI_MMAP_F_PROT             */
    uint32_t flags;         /* TEBOX_MALI_MMAP_F_*                          */
    uint32_t handle;        /* OP_MUNMAP key                                */
    uint32_t reserved;
} __attribute__((packed));

#define TEBOX_MALI_MMAP_F_PROT 1u

/* OP_MUNMAP request: the guest's last VMA over a mapping went away; drop the
 * host mapping (kbase sees the same munmap native libmali would issue). */
struct tebox_mali_munmap_req {
    uint32_t handle;        /* from tebox_mali_mmap_resp.handle */
    uint32_t reserved;
} __attribute__((packed));

/* PCI BAR index carrying the hostmem window (shared-memory capability). */
#define TEBOX_MALI_HOSTMEM_BAR 4
#define TEBOX_MALI_SHM_ID_HOSTMEM 0

#endif /* TEBOX_MALI_PROTO_H */
