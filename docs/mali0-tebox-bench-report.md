# Mali0 / tebox-mali 对比测试报告

- **日期**: 2026-10-21
- **机器**: R1P `10.10.2.142`，guest `aosp-1`（adb `5558`）
- **GPU**: Mali-G720-Immortalis（10 cores），devfreq `performance` @ 546 MHz
- **约束**: 本次只写测试程序与报告；测试代码在 `out/mali0-bench/`，**未改仓库业务代码**
- **原始日志**: `out/mali0-bench/logs/`

## 1. 测什么

| 路径 | 含义 |
|------|------|
| **HOST** | 容器内 CIX `libmali` + GBM，直接打宿主机 `/dev/mali0` |
| **GUEST** | Android 内 NDK 二进制 → guest `libGLES_mali` → `tebox-mali` → 宿主 `/dev/mali0` |

测试程序：`out/mali0-bench/mali0_full.c`  
产物：`out/mali0-bench/bin/mali0_full_android`，宿主侧运行时编译到实例 `/data/mali0_full`（不进 git 源码树）。

同一套用例（默认 N=200）：

1. 5s 吞吐：`glClear×64 + glFinish` FPS  
2. 延迟分布：clear1 / clear64 / 200 draws / 1K 纹理上传 / FBO1080+50draws  
3. fence 等待  
4. buffer 生命周期：256KB churn、8MB churn、64×4MB 批量 alloc/free  
5. 双 EGL context 切换  
6. （仅 guest）AHardwareBuffer → EGLImage  
7. `glReadPixels` 64×64  

另有此前单独跑过的清屏 burn / 微延迟，一并汇总。

## 2. 环境差异（注意）

| | HOST | GUEST |
|--|------|-------|
| DDK / GLES | r54p1 | r49p0 |
| 表面 | GBM window | EGL Pbuffer |
| 内存 | 宿主进程 | QEMU guest（当时约 8G/8smp） |

因此数字是「整条路径」对比，不是严格同二进制同驱动版本对比。

## 3. 吞吐

### 3.1 完整套件内 5s burn（clear×64）

| | frames/5s | FPS |
|--|-----------|-----|
| HOST | 132430 | **26486** |
| GUEST | 91925 | **18385** |

GUEST ≈ HOST 的 **69%**（约慢 31%）。  
（两档 DDK 对“重复 clear”优化可能不同，吞吐只作参考。）

### 3.2 此前同参数独立 burn（1920×1080，clear×64，10s）

| | FPS |
|--|-----|
| HOST | 3052 |
| GUEST | 2934 |

GUEST ≈ HOST 的 **96%**（约慢 **4%**）。

**结论（吞吐）**：简单清屏路径 guest 可以接近宿主；完整套件里差距被放大到约三成，但仍远好于「安兔兔掉到 1/4」那种量级。

## 4. 延迟（µs）— 完整套件

格式：`p50 / p95 / max`（越小越好）

| 用例 | HOST | GUEST | 人话 |
|------|------|-------|------|
| clear1 + finish | 29 / 34 / 36 | 46 / 77 / 154 | 中位约 **1.6×**，尾部更差 |
| clear64 + finish | 36 / 44 / 78 | 44 / 76 / 191 | 中位接近，尾部差 |
| 200 draws + finish | 1811 / 1935 / 2141 | 1784 / 1866 / **28453** | 中位差不多，**max 差一个数量级** |
| tex upload 1K | 661 / 719 / 3634 | 399 / 481 / **53257** | 中位 guest 还快，**偶发尖刺极大** |
| FBO1080 + 50 draws | 513 / 635 / 4967 | 536 / 615 / **19193** | 中位接近，**max ~4×** |
| fence wait | 1 / 14 / 266 | 1 / 1 / 663 | 中位都极低 |
| buf 256K churn | 69 / 91 / 314 | 49 / 98 / **13695** | 平均尚可，**尖刺很大** |
| buf 8M churn | 123 / 169 / 872 | 98 / 148 / **14022** | 同上 |
| dual ctx switch | 76 / 152 / 5371 | 79 / **98355** / **102664** | **灾难项：p95 ~98ms** |
| readpixels 64 | 106 / 112 / 390 | 1 / 1 / 647 | guest 异常快（可能未真正同步读回，谨慎解读） |
| AHardwareBuffer+EGLImage | — | p50 **4260** | 仅 guest；约 4.3ms/次 |

### 批量内存压力（更能打资源路径）

| | HOST | GUEST | 倍率（guest/host） |
|--|------|-------|-------------------|
| 64×4MB alloc | 36.3 ms | **951 ms** | **~26×** |
| 64×4MB free | 0.18 ms | **298 ms** | **~1650×** |

## 5. 此前微基准（补充）

### clear + finish 延迟（N=500 / 200）

| | HOST p50 | GUEST p50 | GUEST/HOST |
|--|----------|-----------|------------|
| clear1 | 28 µs | 39 µs | ~1.4× |
| clear1 p95 | 31 µs | 83 µs | ~2.7× |
| clear1 p99 | 50 µs | 183 µs | ~3.7× |

### 宿主原生 burn 时 mali0 利用率

压测中 `dvfs_utilization` busy 约 **78%**；guest 闲置/轻载时计数器几乎不动、scheduler 常为 `INACTIVE`（计数器对 tebox 上下文不一定可信，以环缓冲/分数为准更稳）。

## 6. 宿主 dmesg（本轮 suite 后）

本轮 suite 结束后未抓到新的 `Enclosing` / `Forbidden` / page-fault（见 `out/mali0-bench/logs/host_after.txt`）。  
历史会话中曾出现：

- `Enclosing region not found … 23 nr_pages` + `mmap failed -12`
- `Attempt to free GPU memory whose freeing by user space is forbidden!`
- （更早错误修复前）`Page fault @0` / `CS_RESOURCE_TERMINATED` / `queue not bound`

这些与 **MEM_FREE / mmap / CSF queue 生命周期**相关，和「偶发 max 尖刺、批量 alloc/free 极慢、双 context 切换炸尾」方向一致。

## 7. 和安兔兔分数的关系（推断）

| 现象 | 量级 |
|------|------|
| 用户反馈 AnTuTu | ~14 万 → ~3.4 万（约剩 **1/4**） |
| 简单 GLES 清屏吞吐 | guest 可到宿主的 **~70%–96%** |
| 中位 draw/FBO 延迟 | 往往 **接近** |
| **尾延迟 / 双 ctx / 大块显存 churn** | guest **差一个数量级到三个数量级** |

**人话**：不是「GPU 算不动」，而是 **复杂场景下的同步与资源生命周期**（多 context、大 BO 分配释放、偶发卡顿）把整机分数打穿。单纯 clear 微基准看不出安兔兔那种崩盘。

## 8. 复现命令

```bash
# guest
adb connect 10.10.2.142:5558
adb push out/mali0-bench/bin/mali0_full_android /data/local/tmp/mali0_full
adb shell /data/local/tmp/mali0_full 200

# host（在 aosp-1 容器内，需已有 /data/mali0_full）
docker exec aosp-1 bash -c '
  source /opt/tebox/scripts/cix-mali-env.sh
  export LD_LIBRARY_PATH=/opt/cixgpu-pro/lib/aarch64-linux-gnu
  export __EGL_VENDOR_LIBRARY_DIRS=/opt/cixgpu-compat/share/glvnd/egl_vendor.d
  /data/mali0_full 200
'
```

编译 guest（仅动 `out/`）：

```bash
NDK=.../ndk/28.2.13676358
$NDK/toolchains/llvm/prebuilt/darwin-x86_64/bin/aarch64-linux-android30-clang \
  -O2 -fPIE -pie -DANDROID -o out/mali0-bench/bin/mali0_full_android \
  out/mali0-bench/mali0_full.c -lEGL -lGLESv3 -lGLESv2 -landroid
```

## 9. 原始文件索引

| 文件 | 内容 |
|------|------|
| `out/mali0-bench/mali0_full.c` | 完整探针源码 |
| `out/mali0-bench/bin/mali0_full_android` | guest 可执行文件 |
| `out/mali0-bench/logs/host_full.txt` | 宿主完整输出 |
| `out/mali0-bench/logs/guest_full.txt` | guest 完整输出 |
| `out/mali0-bench/logs/host_after.txt` | suite 后 dmesg / dvfs / mem |

## 10. 一句话结论

**mali0 硬件没问题；tebox-mali 路径在简单 GL 上够快，但在大块显存 churn、双 context 切换和延迟尖刺上明显落后——这才是更接近安兔兔掉分的方向。**

---

## 11. 代码根因（只读结论，未改仓库）

对照：`src/guest-modules/tebox-mali/tebox_mali.c`（guest ko）+ `qemu/hw/display/tebox-mali.c`（host QEMU）。

### 11.1 一次「开内存」实际在干什么

libmali `MEM_ALLOC(_EX)` → cookie → guest `mmap(cookie)`：

```
guest get_unmapped_area(cookie)
  → virtio OP_MMAP          ← 同步等 QEMU
    → host: first-fit 扫 16GiB bitmap 找 BAR 洞
    → host: mmap(/dev/mali0, cookie)   ← kbase SAME_VA
    → host: memory_region_init_ram_ptr + add_subregion  ← KVM memslot
  ← 返回 gpu_va(=host VA) + hm_off
guest 把 VMA 钉在 gpu_va，remap_pfn_range(BAR+hm_off)
```

`munmap` / `MEM_FREE` 反向：

```
guest vm_close → OP_MUNMAP
  → del_subregion（KVM slot 拆）
  → munmap(kbase) + MAP_FIXED PROT_NONE 占位
  → object_unparent → RCU 后才清 hm_used 位
```

### 11.2 和分数对得上的瓶颈（按杀伤力）

| # | 点 | 代码位置 | 为何炸 churn |
|---|----|----------|--------------|
| 1 | **每个 buffer 一次 KVM memslot** | `tbxm_op_mmap` → `memory_region_add_subregion`；`tbxm_unmap_slot` → `del_subregion` | 64×4MB free 慢 **~1650×** 的主因：slot 抖动 + RCU |
| 2 | **hm_used 释放被 RCU 拖住** | unmap 里 `bitmap_set` 占着洞，clear 在 `tbxm_reclaim_bh` | 窗「假满」→ ENOMEM / enclosing / 重试风暴 |
| 3 | **first-fit 扫整窗** | `tbxm_hm_alloc` 对 `16GiB/4K` bit 线性找洞 | 碎片后 alloc 尾延迟爆炸 |
| 4 | **每次 mmap = 一次 virtio 往返** | guest `tbx_map_create` → `OP_MMAP` | 批量开缓冲 = N 次 kick，无法合并 |
| 5 | **free 路径双 syscall** | `munmap` + `mmap(MAP_FIXED\|ANON)` 占位 | 故意保 KVM 安全，但 free 更贵 |
| 6 | **线性找空槽** | host `TEBOX_MALI_MAX_MAPS` 4096；guest `TBX_MAP_CACHE` 1024 | 高水位 O(n) |
| 7 | **PROT 探测阶梯** | `tbxm_kbase_mmap` 多档 `mmap` 试探 | cookie 未标记时额外失败 mmap |

**重点**：算力路径（clear/draw）已经接近宿主；**极限优化必须打在「开/关内存」整条链上**，目标是把 `bulk_alloc/free 64×4M` 从百毫秒～秒级打回接近宿主的几十毫秒 / 亚毫秒。

---

## 12. 极限优化方案（方案 + 示意代码，默认不落地改仓库）

原则：**少 KVM slot 抖动、少 virtio 往返、O(1) 找洞、free 尽快还窗、批量合并**。按收益排序。

### P0-A — BAR 大小类 freelist（取代 first-fit 扫窗）【极限收益 / 中等改动】

现状：`tbxm_hm_alloc` 每次从 bit0 扫。  
目标：按 size-class（4K / 64K / 256K / 1M / 4M / 8M / 16M / …）O(1) pop/push；大块走 buddy。

```c
/* 示意：qemu/hw/display/tebox-mali.c 新增，替代 tbxm_hm_alloc 热路径 */

#define TBXM_HM_CLASS_N  12
static const uint32_t tbxm_hm_class_pages[TBXM_HM_CLASS_N] = {
    1, 4, 16, 64, 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 /* 4K..128M */
};

typedef struct {
    uint32_t n_pages;
    int32_t  freelist;          /* head index into nodes[], -1 empty */
} TbxmHmClass;

typedef struct {
    int32_t next;               /* freelist link */
    uint32_t off_pages;         /* offset in hostmem pages */
} TbxmHmNode;

typedef struct {
    TbxmHmClass cls[TBXM_HM_CLASS_N];
    TbxmHmNode *nodes;          /* arena of free extents */
    uint32_t nodes_cap, nodes_n;
    /* fallback: still keep hm_used for non-class / coalesced spans */
    unsigned long *hm_used;
    uint64_t hostmem_pages;
} TbxmHmHeap;

static int tbxm_hm_class_of(uint64_t size)
{
    uint32_t need = size / TEBOX_MALI_PAGE;
    for (int i = 0; i < TBXM_HM_CLASS_N; i++) {
        if (need <= tbxm_hm_class_pages[i]) {
            return i;
        }
    }
    return -1; /* oversized → slow buddy/first-fit */
}

static int64_t tbxm_hm_alloc_fast(TeboxMali *m, uint64_t size)
{
    TbxmHmHeap *h = &m->hm_heap;
    int c = tbxm_hm_class_of(size);
    if (c < 0) {
        return tbxm_hm_alloc_slow(m, size); /* rare */
    }
    /* exact class empty → split from larger class */
    for (int i = c; i < TBXM_HM_CLASS_N; i++) {
        if (h->cls[i].freelist < 0) {
            continue;
        }
        int32_t idx = h->cls[i].freelist;
        h->cls[i].freelist = h->nodes[idx].next;
        uint32_t off = h->nodes[idx].off_pages;
        uint32_t got = tbxm_hm_class_pages[i];
        uint32_t need = tbxm_hm_class_pages[c];
        /* push remainder into smaller classes (buddy-split) */
        uint32_t rem_off = off + need, rem = got - need;
        while (rem) {
            for (int j = TBXM_HM_CLASS_N - 1; j >= c; j--) {
                uint32_t sz = tbxm_hm_class_pages[j];
                if (sz <= rem && (rem_off % sz) == 0) {
                    /* push node {rem_off, sz} onto cls[j] */
                    rem_off += sz;
                    rem -= sz;
                    break;
                }
            }
        }
        bitmap_set(h->hm_used, off, need);
        return (int64_t)off * TEBOX_MALI_PAGE;
    }
    return tbxm_hm_grow_from_unused(m, tbxm_hm_class_pages[c]);
}

static void tbxm_hm_free_fast(TeboxMali *m, uint64_t off, uint64_t size)
{
    /* push to class freelist immediately for recycling;
     * KVM slot may still be dying — GPA reuse gated by generation below */
    ...
}
```

**验收**：`lat_buf8M_churn` p95、`bulk_alloc64x4M` 应接近宿主数量级。

---

### P0-B — 消灭「每 buffer 一个 KVM memslot」（极限中的极限）【最大收益 / 大改动】

现状：每个 CPU-visible map = 独立 `ram_ptr` 子 MR → 每次 `add/del_subregion` 打 KVM。  
极限目标：**整窗预先只有 1（或少数）个 memslot**；buffer 只是窗内偏移，不再 per-buffer slot。

两条可选路线：

#### B1. 整窗 memfd + 固定单 slot（推荐冲击极限）

```c
/* 设备 realize 时一次性： */
int mfd = memfd_create("tebox-mali-hostmem", MFD_CLOEXEC);
ftruncate(mfd, hostmem_size);
void *win = mmap(NULL, hostmem_size, PROT_READ | PROT_WRITE, MAP_SHARED, mfd, 0);
memory_region_init_ram_from_fd(&m->hostmem_ram, OBJECT(m),
                               "tebox-mali-ram", hostmem_size, true, mfd, 0, &err);
/* PCI BAR 只挂这一块；之后永不 add/del_subregion 子块 */

/* OP_MMAP 热路径： */
void *kva = tbxm_kbase_mmap(...);          /* 仍要 SAME_VA */
uint64_t hm_off = tbxm_hm_alloc_fast(...);
/* 把 kbase 页填进窗：优先 remap 进窗（见下），否则 copy */
tbxm_publish_into_window(m, hm_off, kva, size);
/* guest 仍 remap_pfn_range(shm_base+hm_off) —— BAR GPA 不变，KVM slot 不变 */
```

SAME_VA 硬约束：guest CPU VA 必须等于 host `mmap(mali0)` 返回的 VA。BAR 只是 guest PA→同一物理页。  
极限做法是 **让 kbase 映射直接落在 window 的固定 VA 切片上**：

```c
/* 预留窗的 VA 空间与 BAR 一一对应（宿主进程）： */
void *win_va = mmap(NULL, hostmem_size, PROT_NONE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);

/* OP_MMAP：在 win_va+hm_off 上 MAP_FIXED 映射 mali fd */
void *p = mmap(win_va + hm_off, size, prot,
               MAP_SHARED | MAP_FIXED, mali_fd, cookie << 12);
/* 则 gpu_va = p，且页已在「逻辑窗」；再用单 slot 的 hostmem_ram 指向同一 memfd
 * 或用 qemu_ram_remap 把该 HVA 登记进已有 slot —— 关键：零次 slot 增删 */
```

> 实现时需验证 CIX kbase 是否允许 `MAP_FIXED` 进预留区；不允许则退化为「单 slot + `process_vm_writev`/临时 copy 进窗」（仍比 slot churn 便宜）。

#### B2. MR 对象池（次极限，改动较小）

预创建常见 size 的 `TeboxMaliBuf`，unmap 时 **不 `object_unparent`**，只 `del_subregion` + 链回 freelist；mmap 时取出、换 `ram_ptr`、再 `add_subregion`。减 malloc/RCU，slot 次数仍在，但路径短很多。

```c
typedef struct {
    TeboxMaliBuf *free_q[TBXM_HM_CLASS_N];
} TbxmBufPool;

static TeboxMaliBuf *tbxm_buf_pool_get(TeboxMali *m, int cls)
{
    TeboxMaliBuf *b = m->buf_pool.free_q[cls];
    if (b) {
        m->buf_pool.free_q[cls] = b->pool_next;
        return b;
    }
    return tbxm_buf_pool_new(m, cls);
}

static void tbxm_buf_pool_put(TeboxMali *m, TeboxMaliBuf *b)
{
    /* del_subregion already done; do NOT unparent */
    int cls = tbxm_hm_class_of(b->size);
    b->pool_next = m->buf_pool.free_q[cls];
    m->buf_pool.free_q[cls] = b;
}
```

**验收**：`bulk_free64x4M` 从 ~300ms → 目标 **&lt;1ms**（B1）或 **&lt;10ms**（B2）。

---

### P0-C — GPA 世代号：RCU 未完也能「逻辑归还」freelist【配合 P0-A】

现状：物理 GPA 必须等 RCU，但 freelist 可以按 **generation** 延迟复用。

```c
typedef struct {
    uint64_t off;
    uint32_t pages;
    uint64_t not_before_gen; /* m->hm_slot_gen at unmap time + 1 */
} TbxmHmDeferred;

/* unmap: */
b->retire_gen = ++m->hm_slot_gen;
tbxm_hm_freelist_push_deferred(m, b->hm_off, b->size, b->retire_gen);

/* alloc: pop freelist only if m->hm_reclaim_gen >= node.not_before_gen
 * else skip to next; never wait in virtio path */
```

RCU BH 只做：`hm_reclaim_gen++` + 真正 `bitmap_clear`。  
这样 **假满** 大幅下降，同时不踩 EEXIST。

---

### P0-D — guest↔host：批量 OP_MMAP / OP_MUNMAP【砍 virtio 次数】

现状：64 个 buffer = 64 次 kick。  
协议扩展（示意，需改 `tebox-mali-proto.h` + ko + qemu，落地时再动）：

```c
/* tebox-mali-proto.h 示意 */
#define TEBOX_MALI_OP_MMAP_BATCH   0x1a
#define TEBOX_MALI_OP_MUNMAP_BATCH 0x1b

struct tebox_mali_mmap_batch_req {
    uint32_t n;               /* ≤ 64 */
    uint32_t reserved;
    struct {
        uint64_t pgoff;
        uint64_t size;
    } items[];                /* flexible */
};
struct tebox_mali_mmap_batch_resp {
    uint32_t n;
    uint32_t reserved;
    struct tebox_mali_mmap_resp outs[];
};
```

guest ko 在 `MEM_ALLOC` 风暴场景（或 `get_unmapped_area` 合并窗口）攒一批再发：

```c
/* guest tebox_mali.c 示意 */
static int tbx_map_create_batch(struct tebox_mali_file *tf,
                                u64 *cookies, u64 *lens, int n)
{
    /* 一次 tebox_mali_cmd(OP_MMAP_BATCH)，填 n 个 map_ent */
}
```

host 侧：

```c
static int tbxm_op_mmap_batch(...)
{
    memory_region_transaction_begin();
    for (i = 0; i < n; i++) {
        tbxm_op_mmap_one(...); /* 内部仍走 freelist；slot 策略见 P0-B */
    }
    memory_region_transaction_commit(); /* 若仍用多 MR，至少合并通知 */
}
```

**验收**：同 64×4MB alloc，virtio 往返 64 → 1～2；guest `bulk_alloc` 目标接近 host×2 以内。

---

### P0-E — MEM_ALLOC 时记下 `va_pages`，mmap 强制精确长度【消 enclosing】

```c
/* note_alloc 时： */
c->cookie_pages[pg - TBXM_COOKIE_FIRST] = va_pages; /* 从 ioctl 结构解析 */

/* tbxm_op_mmap： */
if (cookie_known) {
    uint64_t expect = (uint64_t)c->cookie_pages[idx] << 12;
    if (size != expect) {
        size = expect; /* 或直接 -EINVAL，避免短 mmap 触发 enclosing */
    }
}
```

配合 P0-A 减少「洞被死占」后，`Enclosing region … 23 nr_pages` 应接近消失。

---

### P0-F — free 路径去掉「每次 MAP_FIXED 占位」【在 P0-B1 后】

有单 slot 窗之后，kbase `munmap` 即可；不必再 `mmap(PROT_NONE)` 保 `ram_ptr`。

```c
/* P0-B1 落地后的 unmap 示意 */
static void tbxm_unmap_slot(TeboxMali *m, TeboxMaliMap *mp)
{
    if (!mp->used) return;
    if (mp->host_ptr && mp->host_ptr != MAP_FAILED) {
        munmap(mp->host_ptr, mp->size); /* 仅 kbase VMA */
        mp->host_ptr = NULL;
    }
    tbxm_hm_free_fast(m, mp->hm_off, mp->size); /* 世代号延迟复用 */
    memset(mp, 0, sizeof(*mp));
    /* 无 del_subregion / 无 object_unparent 热路径 */
}
```

---

### P1 — 次要但仍值得（内存相关）

| 项 | 做法 |
|----|------|
| map 槽位 | host/guest 都改成 **空闲栈** 或开放寻址 hash，禁线性扫 4096/1024 |
| cookie_prot | 已有；确保 EX/普通 ALLOC 全覆盖，消灭 PROT 阶梯二次 mmap |
| import 路径 | `OP_IMPORT` 散页拼 memfd 很重；对 Arm gralloc 连续 dmabuf 走 **零拷贝 dma-buf 句柄转发**（另案） |
| 双 ctx | 尖刺来自 ctx switch + 各自 mmap 缓存未热；batch + freelist 后应自降；可加 per-ctx 最近 MR 缓存 |

---

## 13. 落地顺序（极限向，仍先写测试）

1. **先加 out/ 微基准**：只打 `OP_MMAP/MUNMAP` 环（绕开 GL），量「纯开内存」往返。  
2. 落地 **P0-A + P0-C**（freelist + 世代号）→ 复测 `bulk_*` / `buf8M_churn`。  
3. 落地 **P0-D** batch 协议 → 再测。  
4. 最后冲 **P0-B1** 单 slot 窗（最大收益，风险最高）→ 目标 free 路径接近原生。  
5. 全程用 `out/mali0-bench` 回归；**安兔兔只作最终验收**，不作为迭代指标。

### 极限数字目标（相对本报告 host）

| 指标 | 现状 guest | 极限目标 |
|------|------------|----------|
| bulk_alloc 64×4M | 951 ms | **&lt; 80 ms**（≤ host×2.5） |
| bulk_free 64×4M | 298 ms | **&lt; 1 ms** |
| buf8M_churn p95 | 148 µs（偶发 14ms） | p95 **&lt; 200 µs** 且 max **&lt; 2 ms** |
| dual_ctx p95 | ~98 ms | **&lt; 1 ms** |
| Enclosing / Forbidden | 历史有 | **压测 10min 为 0** |

---

## 14. 明确不做什么（避免再踩坑）

- **不要**在 MEM_FREE 热路径注入 `QUEUE_TERMINATE`（已验证打爆活 queue / 分数）。  
- **不要**在 virtio-notify 里 `drain_call_rcu`（软锁/重入）。  
- **不要**USER_IO munmap 时 terminate 队列。  
- 单 slot 方案未验证前，**不要**提前 `bitmap_clear` 复用未 RCU 完的 GPA。

---

## 15. 示意补丁清单（仅文档，未提交）

| ID | 组件 | 摘要 |
|----|------|------|
| P0-A | qemu tebox-mali | size-class freelist 替代 first-fit |
| P0-B1 | qemu tebox-mali | 整窗单 KVM slot + FIXED 映射/发布 |
| P0-B2 | qemu tebox-mali | （过渡）Buf MR 对象池 |
| P0-C | qemu tebox-mali | GPA retire generation |
| P0-D | proto + ko + qemu | `OP_MMAP_BATCH` / `OP_MUNMAP_BATCH` |
| P0-E | qemu tebox-mali | cookie → va_pages 精确 mmap |
| P0-F | qemu tebox-mali | 单 slot 后简化 unmap |
| P1 | ko + qemu | map 槽 freelist；cookie 覆盖 |

**本报告不修改任何现有业务源码；以上代码均为设计示意，供后续按序实现。**
