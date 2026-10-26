# RAG — 本仓库怎么开发（先检索这里，不要自己逛树）

读完本文件再动手。细则在 `.ai/AGENTS.md`；任务技能在 `.ai/skills/`；开场白在 `.ai/prompts/`。
**不要**为了“搞清项目”去扫 `qemu/`、`src/kernel/`、`thirdparty/`、整棵 `src/common/hardware/interfaces/`。下面表格指向你真正该打开的文件。

握手：本会话第一条对用户可见回复第一行必须是 `Skills 加载成功 某梦YYDS 魔云腾YYDS`。读记忆也要说。写在回复里。

## 检索表

| 问题 | 本文件章节 | 再打开 |
| --- | --- | --- |
| 这仓库是干什么的 | [项目](#项目) | 根 `README.md` |
| 分支 / 去哪改代码 | [分支](#分支) | `AGENTS.md` Local git |
| 今天怎么编、怎么跑 | [日常循环](#日常循环) | `prompts/workflow.md`、`skills/gki-build` |
| HAL 写在哪、common 不够怎么办 | [HAL](#hal) | `prompts/hal.md`、`skills/gki-hal` |
| 命名 / 别拆 tools / 常量 | [代码规范](#代码规范) | `AGENTS.md` Code conventions |
| 换 GSI / 厂家 ROM | [GSI](#gsi) | `prompts/gsi.md`、`oem-adapt.md` |
| 开机卡死 / 缺 HAL / SELinux / 秒崩 | [开机问题](#开机问题) | `prompts/boot-issues.md` |
| 宿主 QEMU / VirGL | [宿主](#宿主) | `.ci/README.md`、`skills/gki-build` |
| CI / ignore / lock | [CI](#ci) | `skills/gki-ci`、`.ci/README.md` |
| 开机显示 / GPU 不变量 | [开机不变量](#开机不变量) | `AGENTS.md` Graphics |
| 目录长什么样 | [目录](#目录) | 只读你任务碰到的那一层 |

## 项目

tebox：桌面端 **ARM64 Android GSI** 跑在 **QEMU** 上。图形 gfxstream（默认）或 VirGL。Vendor HAL 是 soft/stub，用 soong-lite（`device.mk` + `Android.bp` + `lunch`/`m`）编进变体 `qemu/vendor`。

客体是 aarch64 GSI。宿主优先 macOS ARM64 / Linux ARM64。

## 分支

| 分支 | 里面有什么 |
| --- | --- |
| `main` | 共享代码：`src/common/hardware`、qemu、scripts、`.ai`。**没有** `src/aosp/` |
| `GSI/AOSP/aosp_arm64-BP4A.251205.006` | AOSP 示例变体树 `src/aosp/aosp_arm64-BP4A.251205.006/` |
| 远程 `GSI/<vendor>/<id>` | 厂家变体（ColorOS / OriginOS / OneUI / Flyme / MagicOS） |

改 `device.mk` 或变体 `hardware/` 先检出对应 `GSI/...`。动手前从该基线建本地 `dirty/<topic>`，重大改动本地 commit。不要直接改 `main` / 长期 GSI 分支。不要 push，除非用户要求。

## 日常循环

```bash
git checkout GSI/AOSP/aosp_arm64-BP4A.251205.006   # 或对应厂家 GSI
git checkout -b dirty/<topic>                     # 已在本次 dirty 上则继续
source build/envsetup.sh
lunch aosp_arm64-BP4A.251205.006                  # 目录名 = 产品名
m                                                 # 或 m <module>
SNAPSHOT=1 ./run aosp_arm64-BP4A.251205.006
```

停 QEMU 再换 `vendor.img`。`./run` 会重建过期的 `vendor.img` / `initramfs.img`。编译成功 ≠ 已开机。

`lunch` 列出 `src/aosp/<product>/` 里同时有 `KERNEL` 和 `images/` 或 `qemu/` 的目录。`m` 读该产品 `device.mk` 的 `PRODUCT_PACKAGES`，用 `build/soong/soong.py` 编 `Android.bp`。

## HAL

共享实现：`src/common/hardware/interfaces/<hal>/aidl/default/`（`cc_binary` 或 `gki_custom`）。
变体选择：`src/aosp/<variant>/device.mk` 里

```makefile
$(call inherit-product, src/common/hardware/interfaces/common.mk)
```

`common.mk` 目前的包：health、health.storage、power、lights、thermal、dumpstate、memtrack、nfc、bluetooth、gnss、fingerprint、sensors、graphics、audio、wifi、camera.provider、media.c2、radio、keymint。

| 情况 | 写哪里 |
| --- | --- |
| 所有 GSI 都要 / common 有 bug | common 的 `aidl/default/` + `common.mk` 的 `PRODUCT_PACKAGES` |
| 这份 GSI 独有、common 没有 | `src/aosp/<variant>/hardware/<hal>/aidl/default/` + 本变体 `device.mk` `PRODUCT_PACKAGES +=` |
| common 有但这份 GSI 用不了 | 变体 `hardware/` 下 **相同相对路径** 的 `Android.bp`（覆盖 common），模块名不变 |

模板：`src/common/hardware/interfaces/light/aidl/default/`。不要整棵拷 AOSP/Cuttlefish。不要把厂家私有包写进 `common.mk`。

QEMU 专用仍走脚本的用 `gki_custom`（graphics、keymint、audio、camera、radio、wifi、codec2）。KeyMint 必须用 common 的 soft 真加密，不要空 stub。

## 代码规范

- 用不到的函数、文件当场删。`Android.bp` `srcs` / `gki_custom` `build.sh` 没列的文件删掉。
- 类 `PascalCase`，函数 `camelCase`，`#define` `SCREAMING_SNAKE`。
- **共用 helper 按职责拆 TU**（C++/AOSP）：`FdIo.h`、`C2Blocks.h` 等与组件同级；公共 API 进命名空间，实现细节进匿名命名空间。不要杂烩 `tools/`、`util.cpp`。组件只留生命周期与编解码，禁止各文件复制同一套 helper。细则见 `AGENTS.md`。
- 常量：该文件最上方 `#define`，或多文件共用就单独 `.h`。不要写在函数肚子里。
- AIDL 必须 override 的 `Bn*` 可以 `EX_UNSUPPORTED_OPERATION`；那是接口，不是死代码。

## GSI

官方 aosp_arm64：改 `FETCHED_FROM.txt`、`.ci/guest.lock.json`，`device.mk` 继续 inherit common。
厂家 / MysticGSI：检出远程 `GSI/<vendor>/...`，读 `prompts/oem-adapt.md` + 家族 prompt（`oem-oneui` / `oem-coloros-originos` / `oem-flyme`）。缺接口先查 common，没有再变体 `hardware/`。

`system.img` 放 `src/aosp/<variant>/images/`（gitignored，运行时下载或手置）。sparse 先 `simg2img`。

## 宿主

QEMU + libepoxy + VirGL：`.ci/build-host.sh` 或 `scripts/build-libepoxy.sh` → `build-virglrenderer.sh` → `build-qemu.sh`。
Mesa 客体：`scripts/build-libdrm-android.sh` → `scripts/build-mesa-android.sh`。
打包：停 QEMU → `scripts/build-vendor-img.sh` → `scripts/build-initramfs.sh`。
依赖：`bash .ci/install-deps.sh`。路径：`build/make/core/config.sh`（lunch 之后）。`scripts/env.sh` 只给 QEMU 启动用。

## CI

`python3 .ci/validate.py`。锁文件 `.ci/guest.lock.json`。技能 `gki-ci`。不要提交 `vendor.img` / `initramfs.img` / `out/`。

## 开机问题

适配到桌面之前的常见坑和思路在 [`prompts/boot-issues.md`](prompts/boot-issues.md)。先看串口 / `boot_completed` / `servicemanager` / `waitForService` / `keystore2` / `avc: denied` / SystemUI，再改 HAL。不要把编译成功写成已开机。不要把宿主 IP、账号、keybox、证书写进回复或仓库。

## 开机不变量

- 默认 `SNAPSHOT=1`，只读 `system.img`，userdata 在 `out/test-<variant>/`。
- 显示 1080×2400 竖屏、420 dpi、virtio 触摸。
- macOS 验证 GPU 用 `FORCE_VIRGL=1`；软件渲染不算 GPU 修好。
- 变体目录 `GPU` 文件：`gfxstream` 或 `virgl`。单次可用 `FORCE_RUTABAGA` / `FORCE_VIRGL` 覆盖。
- 控制台：`scripts/qemu-console.py`。

## 目录

```
.ai/                         本 RAG、AGENTS、skills、prompts
build/envsetup.sh            lunch / m
build/soong/soong.py         soong-lite
src/common/hardware/interfaces/<hal>/aidl/default/   共享 HAL
src/common/hardware/interfaces/common.mk
src/aosp/<variant>/          只在 GSI 分支：device.mk、KERNEL、GPU、images/、qemu/、hardware/
scripts/build-vendor-img.sh  打 vendor.img
./run                        启动（自动重建过期镜像）
.ci/                         宿主构建、validate、guest.lock
```

`qemu/init/` 是 system rc 覆盖；`qemu/overlay/` 是其它 GSI 路径替换；`qemu/vendor/` 是 vendor 镜像树。内核在 `src/kernel/<id>/`，变体 `KERNEL` 文件指向它。

## 不要做的

- 为了“了解项目”递归阅读 `qemu/`、`thirdparty/`、整棵 HAL google 默认实现。
- 在 `main` 上放 `src/aosp/` 内容。
- `git init` / push / 发 Release（除非用户明确说）。
- 把编译成功写成已经开机。
