# 开机常见问题（适配 GSI 到能进桌面之前）

先读 `.ai/RAG.md`、`.ai/AGENTS.md`。本页是 **QEMU 上把 ARM64 GSI 拉到 `sys.boot_completed=1` + 桌面** 时反复出现的坑和思路。厂家细节再开 `oem-*.md`。

不要把编译成功写成已开机。不要记录或复述宿主 IP、账号、证书序列号、keybox 私钥、机身 DeviceID。

## 怎么查（先这条再改代码）

1. QEMU 是否还在、内核有没有 panic（`scripts/qemu-console.py` / 串口）。
2. ADB 能否连上（本仓库默认 hostfwd 是本机环回 `5555`，以当时 `./run` 为准）。
3. `getprop sys.boot_completed` 是否为 `1`。
4. `logcat` 里谁在重启：`init`、`servicemanager`、`zygote`、`system_server`、`SystemUI`、`keystore2`。
5. 按下面分类对号：缺 HAL / 死等私有服务 / KeyMint / SELinux / 身份与 init / props 秒崩 / 图 / 内核。
6. 改完停 QEMU，`lunch && m`（或只 `m <module>`）再打 `vendor.img` / `initramfs.img`，`SNAPSHOT=1` 再开。

日志关键词：

| 看到 | 先当 |
| --- | --- |
| `Could not find … in the VINTF manifest` / `servicemanager` 找不到 | 缺 HAL 或 instance 名不对 |
| `waitForService` 卡住、某个 vendor 进程反复起 | OEM 私有 AIDL，common 没有 |
| `keystore2` / KeyMint / `Failed to generate` | 空 stub 或 rc 冲突 |
| `avc: denied` | SELinux；bring-up 先 permissive |
| SystemUI 秒退、zygote 崩 | 音量步长 / 动画档 / fingerprint 打架 |
| SurfaceFlinger / mapper / composer | 图形 HAL / GPU 文件 |
| 内核起不来、virtio 模块没有 | `KERNEL` 与 GSI 不配 |
| ADB 突然没了、audit 刷屏 | 在没改 sepolicy 时强行 enforcing |

## 缺 HAL / VINTF

**现象：** framework 起了但卡在 boot；`servicemanager` 报找不到 AIDL；`init` 把某个 `android.hardware.*` 杀掉再拉。

**思路：**

- 对照 FCM / `out/gsi-vintf` 允许的 **AIDL 版本**，不要发明矩阵没有的 version。
- instance 名必须对上：多数 `/default`；相机 FCM 常见 `ICameraProvider/internal/0`（不是随便改成 `/default`）。
- 先看 `common.mk` 有没有这个包。有：确认编进了当前变体 `qemu/vendor`，rc 在跑，`manifest.xml` 有 fqname。
- 没有或不适用：变体 `hardware/` 自己写 + `device.mk` `PRODUCT_PACKAGES +=`（见 `prompts/hal.md`）。
- VINTF XML 里的 `version="1.0"` 是 **清单 schema**，不是 HAL API 版本。
- 不要手改 soong 装进去的 HAL `*.rc` / `*.xml`；改源码再 `m`。

## waitForService / OEM 私有接口

**现象：** 相册、电话、设置、Knox、充电动画卡死；log 里某个 `vendor.*.I*` / `Seh*` / `oplus` / `vivo` 一直 wait。

**思路：**

- 这不是 common 的活。在 **本变体** `hardware/` 做最小 stub（空实现 + VINTF + rc），让 wait 返回。
- 已知家族（细节在对应 oem prompt）：
  - OneUI：KnoxGuard、DisplayQuality、KHDM、SehKeyMint、SehWifi、SehHyPer/DVFS、SemRIL 等。
  - ColorOS / OriginOS：`vendor.oplus.hardware.charger`、`vivo_daemon`、vivo-algo（`IVASAlgoProvider`，算法可空）。
- 不要为了“完整”拷整份厂商 HAL。boot 不需要的方法 `EX_UNSUPPORTED_OPERATION` 即可。
- 先 permissive 确认是 wait 而不是 AVC 把 stub 卡死。

## KeyMint / keystore2

**现象：** 到不了桌面；`keystore2` 崩；锁屏/密钥相关 ANR。空 KeyMint stub **不能** 完成开机需要的密码学。

**思路：**

- 用 common 的 soft KeyMint（`security/keymint/.../soft`），不要空 stub。
- 系统分区若自带 KeyMint rc，用 `qemu/init/` 覆盖或 initramfs bind **遮掉冲突的 system rc**，只留 vendor 这一份。
- 若 GSI 读 `/vendor/etc/keymint/keybox.xml`：按镜像需要放置；只有 ECDSA、没有 RSA 时，RSA attestation 走 AOSP 内置 software 密钥是预期，不是“keybox 坏了”。
- 认证 App 显示「未知根证书」常常是 **App 根证书白名单**，不是 HAL 没加载。先看 keystore2/HAL 日志。
- 不要把 keybox、证书链、DeviceID 写进文档或 commit。

## SELinux

**现象：** permissive 能进桌面，enforcing 进不去；或一改 enforcing ADB 断、audit 刷屏。

**思路：**

- 适配前期：`androidboot.selinux=permissive`，先把 HAL/props 做完。
- 很多厂家 GSI 的 init **从不** `setenforce(1)`。只改 cmdline 不够时，要 early-init 写 `/sys/fs/selinux/enforce`，并且 **vendor sepolicy 给 root/adbd 用的域**（常是 `su`）真正允许它在 enforcing 下跑。
- **不要**在没改 sepolicy 时对正在跑的 guest `echo 1 > /sys/fs/selinux/enforce`：`su` 可能无法 accept ADB，连接掉光。
- `plat_sepolicy_vers` 跟镜像（如 34.0 / 202404），不要抄错版本。
- `boot_completed=1` 且桌面已出之后，个别 OEM 包（安全中心、IMS、蓝牙）仍崩、仍有 AVC，**不一定**还是开机阻塞。先记日志，别为它们打转 HAL。

## 设备身份 / init 覆盖

**现象：** 没有 `init.ranchu.rc`；厂商服务找 qcom 节点；vulkan/composer 找不到对应 so。

**思路：**

- AOSP 示例常用 `ranchu`。ColorOS / OriginOS 一类常要 `androidboot.hardware=qcom`、`ro.board.platform`，必要时 `vulkan.ranchu.so` → `vulkan.qcom.so`（变体 `GPU` / qemu 配置）。
- 无 ranchu init：`qemu/vendor` 占位 + initramfs bind，**或** `qemu/init/bake-overlays` 写进 system。选一种写进 `FETCHED_FROM`，不要两条各改一半。
- 分层：`qemu/init/` 只放 system `.rc` 覆盖；`qemu/overlay/` 放其它 system 路径替换；`qemu/vendor/` 才是 vendor 树（fstab、sepolicy、props）。
- OneUI 一类优先 initramfs bind（platform.xml、vndk.rc、launcher URI、theme、mask virtual_camera），一般不 bake `system.img`。

## Props 秒崩

**现象：** zygote / SystemUI 起来立刻死循环；音量条一拉就崩。

**思路：**

- **音量步长为 0 → 除零：** 设 `ro.config.media_vol_steps`（以及 ring/alarm 等）和合理 default。SNAPSHOT 开机后流音量可能回到默认，测音频时再 `cmd audio set-volume`。
- 厂商动画档（如 oplus `persist.sys.oplus.anim_level`）有已知崩溃值，避开；必要时 boot 后强制安全值。
- `ro.vndk.version` / `ro.product.first_api_level` 跟镜像走。
- vendor `fingerprint` 不要和 system 打架，否则 zygote/overlay 异常。
- OneUI：`ro.config.knox` 给安全的占位（如 `v00`）；密度跟项目默认 420，除非任务改分辨率。
- logd/vold `library not found`：查 `ld.config` 是否包含 `/system_ext` 搜索路径。

## 图形 / 显示

**现象：** 黑屏、花屏、SurfaceFlinger 重启；软件渲染能出图、GPU 不能。

**思路：**

- 变体目录 `GPU` 文件：`gfxstream` 或 `virgl`。单次用 `FORCE_VIRGL` / `FORCE_RUTABAGA` 验证。
- 软件渲染只能当诊断，不能当「GPU 修好了」。macOS 验证 GPU 用 VirGL。
- 保持 1080×2400 竖屏、420 dpi、virtio 触摸，除非任务明确改。
- mapper / composer 链接 GSI 的 `libbinder_ndk`；不要把 `libc++_shared` 乱塞进 mapper SPHAL 路径。
- Android 14 上切 gfxstream 注意符号兼容，用 `FORCE_*` 单次试，不要默默改金标 GPU。

## 内核 / 镜像文件

**现象：** 停在 bootloader 风格日志、virtio 盘/网起不来、GSI 要更新的 GKI。

**思路：**

- 读变体 `KERNEL` → `src/kernel/<id>/`。FCM/内核要求跳了（例如 6.1→6.12）先换预编译 Image/模块和 lock，再查 framework。
- `system.img` 若是 sparse，先 `simg2img`。手置厂家包，记 sha256 到 `FETCHED_FROM`。
- 超大 ZIP64 厂家包：系统自带解压器可能失败，用能认 ZIP64 的 `bsdtar` / Python `zipfile`，不要死磕图形解压。

## 音频（桌面已出、声音不对）

开机成功之后才查。常见：

- `IModule/default` 必须是 **PRIMARY 扬声器**（带 PCM16/48 kHz profile），不要把 STUB/AFE_PROXY 当成 default，否则 APM mix 为空。
- r_submix **只声明 48 kHz**。声明 8k–192k 时 AudioFlinger 可能按 192 kHz 开 mixer，和 48 kHz 采集对不上，音调错、滋啦。
- 空 engine config 会让 `STREAM_MUSIC` maxIndex=0。音量组名要对上 `AUDIO_STREAM_*`。
- 环回/采集管道在 start 时 flush；不要用超大 ring + 长时间 IN pace 造成秒级延迟。
- SNAPSHOT 会丢掉运行时音量。

## 相机（桌面已出）

QEMU 上没有真实 `/dev/video*`。不要为了相机去加宿主 vsock 设备；用 HAL 模拟。FCM 实例常见 `ICameraProvider/internal/0`。provider 可在 `sys.boot_completed` 之后再起并重启 cameraserver。

## 什么叫「开机成功」

同时满足再往下做功能：

- `sys.boot_completed=1`，`system_server` 稳定
- 桌面/Launcher 出来，ADB 可用
- 分辨率/密度符合不变量（除非任务改了）
- 阻塞 boot 的 HAL 已 `Found` / `registered`
- 编译过、镜像打过、**这轮 guest 实测过**

个别 OEM 进程仍崩、仍有 AVC，只要不挡桌面，记下来即可，不要和「没开机」混为一谈。
