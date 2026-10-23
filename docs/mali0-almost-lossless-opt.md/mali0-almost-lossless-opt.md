# tebox-mali 几乎无损优化方案（对齐 VirGL / gfx 高性能内存面）

- **目标**：guest 经 tebox-mali 打 `/dev/mali0` 的 **开/关内存 + mmap** 接近宿主直开 mali0（「几乎无损」≈ alloc/free **&lt;2×** 原生，热路径 **KVM memslot 增删 = 0**）。
- **依据**：本树 VirGL `map_fixed`、rutabaga/gfxstream QEMU 路径、现有 `tebox-mali`、以及 `mali0-tebox-bench-report.md` 实测。
- **约束**：本文只做设计；**不改现有业务代码**。示意代码仅文档。

---

## 0. 一句话

**学 VirGL 快路径，别学本树 QEMU-rutabaga 的慢路径。**

VirGL 已经验证：**整窗预先注册 1 个 KVM slot + `MAP_FIXED` 换页** = map/unmap 不碰 FlatView/KVM。  
tebox-mali 今天 = rutabaga 慢路径（每 buffer `init_ram_ptr` + `add/del_subregion`）+ **每次 mmap 一次 virtio** → free 慢三个数量级。

---

## 1. 业界高性能做法（本树实码）

### 1.1 VirGL 快路径（必须抄）

**Realize：整窗一次进 KVM**

```145:159:qemu/hw/display/virtio-gpu-gl.c
    map = qemu_ram_mmap(-1, b->conf.hostmem, ...);
    gl->hostmem_mmap = map;
    memory_region_init_ram_ptr(&gl->hostmem_background, NULL,
                               "hostmem-background", b->conf.hostmem,
                               gl->hostmem_mmap);
    memory_region_add_subregion(&b->hostmem, 0, &gl->hostmem_background);
```

**Map：`MAP_FIXED` 钉进预留 HVA —— 零 `add_subregion`**

```189:194:qemu/hw/display/virtio-gpu-virgl.c
    ret = virgl_renderer_resource_map_fixed(res->base.resource_id,
                                            gl->hostmem_mmap + offset);
    case 0:
        res->map_fixed = gl->hostmem_mmap + offset;
        return 0;   /* 不创建 MemoryRegion */
```

virglrenderer：

```c
/* thirdparty/virglrenderer — dmabuf/shm → 指定地址 */
map = mmap(addr, size, PROT_READ|PROT_WRITE, MAP_FIXED|MAP_SHARED, fd, 0);
```

**Unmap：同地址 `MAP_FIXED` 盖匿名页 —— 零 `del_subregion`**

```247:257:qemu/hw/display/virtio-gpu-virgl.c
    if (res->map_fixed) {
        mmap(res->map_fixed, res->base.blob_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        res->map_fixed = NULL;
    }
```

| 操作 | KVM `SET_USER_MEMORY_REGION` | FlatView 重建 |
|------|------------------------------|---------------|
| realize 一次 | 1 | 1 |
| 每个 blob map/unmap | **0** | **0** |

Guest 侧只做：窗内 `drm_mm` 选偏移 → `MAP_BLOB` → `io_remap_pfn_range(BAR+off)`。

### 1.2 VirGL 慢路径（明确当反面教材）

`map_fixed` 不支持时回退：

```211:227:qemu/hw/display/virtio-gpu-virgl.c
    virgl_renderer_resource_map(... &data, &size);
    memory_region_init_ram_ptr(mr, ..., size, data);
    memory_region_add_subregion_overlap(&b->hostmem, offset, mr, 1);
```

Unmap 要 `del_subregion` + RCU + 阻塞 cmdq。VirGL 自己也觉得这是 fallback。

### 1.3 本树 QEMU-rutabaga / gfxstream（现状 = 慢路径）

```860:880:qemu/hw/display/virtio-gpu-rutabaga.c
    rutabaga_resource_map(..., &mapping);
    memory_region_init_ram_ptr(mr, ..., mapping.size, mapping.ptr);
    memory_region_add_subregion(&vb->hostmem, mblob.offset, mr);
```

- 上限 `MAX_SLOTS=4096`，**每 resource 一次 slot**。
- 库侧其实已有 `map_placed` → `virgl_renderer_resource_map_fixed`（`rutabaga_gfx/.../virgl_renderer.rs`），**本树 QEMU 没接**。
- gfxstream backend 多返回任意 HVA，无 fixed；QEMU 只能包成子 MR。

→ **不要把 rutabaga QEMU 现状当终态**；要抄的是 VirGL `hostmem_background` + fixed。

### 1.4 udmabuf / GUEST blob（方向相反，别误用）

virtio-gpu `blob_mem=GUEST` + udmabuf：guest RAM 页合成 dmabuf 给宿主 GPU。  
tebox-mali 是 **宿主 kbase 页 → guest CPU SAME_VA**，方向相反。udmabuf **不能**替代整窗 fixed 方案；最多以后做 gralloc import 旁路。

### 1.5 gfx「高性能」在命令面（内存面之外）

VirGL/gfxstream 另有：cmd 批处理、fence 异步、blob 跨设备 share。  
对 tebox-mali「几乎无损」：**内存面先抄 VirGL fixed；命令面再加 batch mmap/ioctl**（见下）。算力微基准已经接近原生，优先打 churn。

---

## 2. tebox-mali 今天差在哪（对照）

```
VirGL fast:
  [realize]  qemu_ram_mmap(N) ──► ONE KVM slot
  [map]      mmap(MAP_FIXED → hostmem_mmap+off)   // no add_subregion
  [unmap]    mmap(MAP_FIXED anon → same HVA)      // no del_subregion

tebox-mali today (≈ rutabaga slow):
  [realize]  空 hostmem 容器（无 background RAM）
  [mmap]     mmap(mali0 任意 VA) + init_ram_ptr + add_subregion  // +1 slot
  [munmap]   del_subregion + PROT_NONE 占位 + RCU 后才还 hm_used // −1 slot
  [guest]    每次 get_unmapped_area/mmap = 一次 OP_MMAP virtio
```

实测锚点（`mali0-tebox-bench-report.md`）：

| 指标 | guest vs host |
|------|----------------|
| clear/draw 中位 | ~1–1.5×（可接受） |
| bulk_alloc 64×4M | **~26×** |
| bulk_free 64×4M | **~1650×** |

**几乎无损 = 必须把 free/alloc 从「slot 抖动路径」迁到「VirGL fixed 路径」。**

---

## 3. 目标架构：「mali-hostmem-background + MAP_FIXED」

### 3.1 不变量

1. **SAME_VA**：guest 用户态指针 == GPU VA == 宿主 `mmap(mali0)` 返回值。  
2. **零拷贝**：guest 通过 BAR `remap_pfn_range` 碰到的物理页 == kbase 映射页。  
3. **热路径零 KVM slot 增删**（仅设备 realize 注册整窗一次）。

### 3.2 Realize（抄 VirGL）

```c
/* 示意：tebox-mali PCI realize —— 文档用，未落地 */
void *win = qemu_ram_mmap(-1, hostmem_size, qemu_real_host_page_size(), 0, 0);
/* 或：mmap(NULL, size, PROT_NONE, MAP_PRIVATE|MAP_ANONYMOUS|MAP_NORESERVE, -1, 0)
 *     再按需 MAP_FIXED 填页 —— 与 VirGL 等价思想 */

m->hostmem_mmap = win;
memory_region_init_ram_ptr(&m->hostmem_background, OBJECT(m),
                           "tebox-mali-hostmem-bg", hostmem_size, win);
memory_region_add_subregion(&m->hostmem, 0, &m->hostmem_background);
/* PCI BAR 仍指向 m->hostmem；此后禁止为单个 buffer add/del_subregion */
```

窗内偏移分配：size-class / buddy freelist（O(1)），**废弃** 热路径 first-fit 扫 16GiB bitmap。

### 3.3 OP_MMAP 热路径（对齐 `resource_map_fixed`）

```c
/* 示意 */
uint64_t hm_off = tbxm_hm_alloc_fast(m, size);
void *addr = m->hostmem_mmap + hm_off;

/* 关键：kbase 页直接落在窗切片上 → SAME_VA == 窗 VA */
void *p = mmap(addr, size, prot, MAP_SHARED | MAP_FIXED,
               c->fd, (off_t)(cookie << 12));
if (p == MAP_FAILED) {
    /* 见 §3.5 降级 */
    return tbxm_mmap_fallback_slow(...);
}
assert(p == addr);

/* ★ 禁止 memory_region_init_ram_ptr / add_subregion */

resp.gpu_va = (uint64_t)(uintptr_t)p;   /* == hostmem_mmap + hm_off */
resp.hostmem_offset = hm_off;
resp.handle = alloc_handle(m, ...);
/* guest 协议不变：remap_pfn_range(shm_base + hm_off) */
```

### 3.4 OP_MUNMAP（对齐 VirGL unmap_fixed）

```c
void *addr = m->hostmem_mmap + mp->hm_off;
/* 先让 kbase 释放：对 mali fd 的映射就是这段 MAP_FIXED 区 */
munmap(addr, mp->size);   /* 或先 MAP_FIXED 盖 anon 再视 kbase 语义调整 */

/* 用匿名页盖回，保持「窗 HVA 范围」对 KVM 合法（与 VirGL 相同） */
mmap(addr, mp->size, PROT_READ | PROT_WRITE,
     MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);

tbxm_hm_free_fast(m, mp->hm_off, mp->size);  /* 立即入 freelist */
/* ★ 禁止 del_subregion / object_unparent 热路径 */
```

### 3.5 若 kbase 拒绝 `MAP_FIXED` 进预留窗（降级阶梯）

| 优先级 | 做法 | 无损程度 |
|--------|------|----------|
| 1 | `MAP_FIXED` 进 `hostmem_mmap+off`（首选） | 几乎无损 |
| 2 | 任意地址 `mmap(mali0)` 后 `mremap(..., MREMAP_FIXED\|MREMAP_MAYMOVE, addr)` | 接近无损 |
| 3 | 任意 mmap + **仅**把页写入已注册窗（copy/`process_vm`） | 有损，仅应急 |
| 4 | 维持今日 per-buffer MR（现状） | 不可接受 |

实现前用 `out/mali0-bench` 在容器内对 `/dev/mali0` 做 **MAP_FIXED 探针**（只测宿主，不经 QEMU）确认 CIX r49/r54 行为。

### 3.6 与 VirGL 的差异点（必须处理）

| | VirGL blob | tebox-mali |
|--|------------|------------|
| 宿主 backing | dmabuf/shm fd | **mali0 cookie mmap** |
| VA 约束 | 任意窗偏移即可 | **guest VA == host mmap VA** |
| Guest 布局 | guest `drm_mm` 选 off | host 分配 off，并令 `gpu_va = win+off` |

因此 freelist 选出的 `hm_off` 必须同时满足：窗对齐 + **该切片 VA 在 guest 进程里可 `MAP_FIXED` 放置**（今日 `get_unmapped_area` 已检查交叉；SAME_VA 落窗后 guest 与 host 用同一数值）。

多进程 guest：每进程自己的 VA 空间要能放下 `win+off`。现有模型已要求 guest 使用 host 返回的 VA；改成窗基址后，**窗的宿主 VA 基址应选在 guest 宽松区**（例如与今日 kbase top-down 习惯兼容的高地址），或 per-ctx 子窗。极限方案可 **每 ctx 一块子窗**（仍是少量 slot，不是每 buffer）。

---

## 4. 命令面：批量化（第二刀，配合内存面）

VirGL/gfx 资源创建频率远低于 GL buffer churn；tebox 每个 `mmap` 都同步 kick。

```c
/* 协议示意 */
TEBOX_MALI_OP_MMAP_BATCH / OP_MUNMAP_BATCH  /* n≤64 */
```

host 处理 batch 时：**无** `memory_region_transaction` 包一堆 add_subregion（因为根本不该再有）；只循环 `MAP_FIXED` + freelist。

guest：`MEM_ALLOC` 风暴或显式 flush 窗口合并。

目标：64×4MB alloc 的 virtio 往返 64 → 1。

---

## 5. 明确不要做的（踩坑清单）

| 禁止 | 原因 |
|------|------|
| 终态保留 per-buffer `add/del_subregion` | 与 rutabaga 慢路径同病；自由无上限 |
| virtio-notify 里 `drain_call_rcu` | 软锁 / 重入 |
| MEM_FREE 热路径乱 `QUEUE_TERMINATE` | 已实测打爆活 queue / 分数 |
| 用 udmabuf「代替」SAME_VA 窗 | 方向反了 |
| 把 gfxstream 任意 HVA map 当快路径 | 没有 fixed 就会退化成子 MR |
| 未验证 MAP_FIXED 前就 `bitmap_clear` 复用未 RCU GPA | EEXIST abort（旧坑）—— 新架构下应 **根本不拆 slot** |

---

## 6. 落地里程碑（几乎无损验收）

全部用 `out/mali0-bench`；**不改业务代码直到探针证明 MAP_FIXED 可用**。

| 阶段 | 内容 | 验收（相对宿主直开 mali0） |
|------|------|---------------------------|
| M0 | 容器内 `mali0` `MAP_FIXED` 探针 | 固定地址 mmap/munmap 稳定 |
| M1 | QEMU：`hostmem_background` + freelist；mmap 走 fixed；禁子 MR | `KVM_SET_USER_MEMORY_REGION` 热路径 = 0；`bulk_free` **&lt;5×** |
| M2 | `OP_MMAP_BATCH` | `bulk_alloc` **&lt;2.5×** |
| M3 | cookie→va_pages / PROT 无探测；双 ctx 回归 | enclosing/forbidden 长稳 0；dual_ctx p95 **&lt;1ms** |
| M4 | AnTuTu 终验 | 接近历史正常分，而非 1/4 |

### 数字锚点（几乎无损）

| 指标 | 今 guest | 目标 |
|------|----------|------|
| bulk_alloc 64×4M | 951 ms | **&lt; 80 ms**（≤ host×2.5） |
| bulk_free 64×4M | 298 ms | **&lt; 1 ms** |
| 热路径 KVM slot ± | 每 buffer ±1 | **0** |
| clear1 p50 | ~1.4–1.6× | 保持或更好 |

---

## 7. 示意：最小「VirGL 化」补丁轮廓（仅文档）

```text
qemu/hw/display/tebox-mali.c
  + hostmem_mmap / hostmem_background   // 抄 virtio-gpu-gl.c
  + tbxm_hm_alloc_fast / free_fast      // size-class
  ~ tbxm_op_mmap: MAP_FIXED into window; DELETE add_subregion path
  ~ tbxm_unmap_slot: MAP_FIXED anon restore; DELETE del_subregion hot path

qemu/hw/display/tebox-mali-proto.h + guest ko
  + OP_MMAP_BATCH / OP_MUNMAP_BATCH     // 第二阶段

out/mali0-bench/
  + mali0_map_fixed_probe.c             // M0 探针（先写这个）
```

---

## 8. 总结

| 来源 | 带走什么 |
|------|----------|
| **VirGL** | `hostmem_background` + `map_fixed` / unmap 盖页 → **几乎无损内存面的标准答案** |
| **rutabaga QEMU 现状** | per-slot MR → **反面教材**；库有 `map_placed` 可作远期对齐 |
| **gfxstream** | 命令批处理思路；内存面勿照搬任意 HVA map |
| **tebox 今日** | 算力够用；必须把 mmap/munmap 从「rutabaga 慢路径」迁到「VirGL 快路径」 |

**几乎无损 = 1 个 KVM slot 管整窗 + mali0 映射 `MAP_FIXED` 进窗 + freelist O(1) +（可选）batch virtio。**  
其余优化都是锦上添花；不换内存面模型，安兔兔级 churn 打不满。
