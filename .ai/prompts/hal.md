先读根目录 README.md，再读 .ai/RAG.md（HAL 章节）与 .ai/AGENTS.md，使用 gki-hal 技能开发或修补 vendor HAL。
本会话第一条对用户可见回复的第一行必须是：`Skills 加载成功 某梦YYDS 魔云腾YYDS`。读了记忆也要说；写在回复里，思考不算。

当前是 **product makefile + lunch / m**，不要再走变体 `hardware/*/stub/build-stub.sh` 当默认路径。

## 先确认树

GSI 变体在对应 `GSI/...` 分支（AOSP 示例：`GSI/AOSP/aosp_arm64-BP4A.251205.006`）。`main` 没有 `src/aosp/`。改 `device.mk` 或变体 `hardware/` 前先检出该分支，再从其上建本地 `dirty/<topic>`。每完成一块能回滚的改动就本地 commit（不要 push）。

```bash
source build/envsetup.sh
lunch <variant>          # 目录名，如 aosp_arm64-BP4A.251205.006
m                        # 编 PRODUCT_PACKAGES 里全部 HAL
m android.hardware.lights-service   # 只编一个
SNAPSHOT=1 ./run <variant>
```

`m` 读 `src/aosp/<variant>/device.mk`（`inherit-product` + `PRODUCT_PACKAGES`），用 soong-lite 编 `Android.bp`，装进该变体 `qemu/vendor`。停 QEMU 后再打 `vendor.img`（或 `./run` 自动重建）。

## 先用 common，不够再写自己的

共享 HAL 在 `src/common/hardware/interfaces/<hal>/aidl/default/`（`cc_binary` 或 `gki_custom`）。变体用：

```makefile
# src/aosp/<variant>/device.mk
$(call inherit-product, src/common/hardware/interfaces/common.mk)
```

**所有 GSI 都需要、或 common 实现有 bug** → 改 common，并把模块写进 `common.mk` 的 `PRODUCT_PACKAGES`。不要把厂家私有接口塞进 common。

**common 没有这个接口，或这份 GSI 用不了 common 那份（AIDL 版本、额外 OEM 方法、不同二进制）** → 只在本变体写：

1. 新模块（common 里没有同名）：
   - 源码：`src/aosp/<variant>/hardware/<hal>/aidl/default/`
   - `Android.bp`（`cc_binary` / `gki_custom`）+ `*.rc` + VINTF `*.xml`
   - 在 **本变体** `device.mk` 里 `PRODUCT_PACKAGES += <module>`
2. 覆盖 common 已有模块：在变体里放 **相同相对路径** 的 `Android.bp`  
   （相对 `src/common/hardware/interfaces/` = 相对 `src/aosp/<variant>/hardware/`）。soong-lite 用变体文件替换 common。模块名保持一致。

对照 `src/common/hardware/interfaces/light/aidl/default/`（`Android.bp` + rc + xml + cpp）。QEMU 专用、仍用脚本的用 `gki_custom`（见 graphics / keymint / audio）。

不要把整个 common 拷进变体。不要改 `common.mk` 去加只服务一家的包。

## 代码规范（必须）

当前 HAL 树已经很臃肿。每次改完 **删掉用不到的函数和文件**，不要再往里堆。

- `Android.bp` 的 `srcs`、rc、xml 里没有的文件删掉。`cc_binary` 已经编过的，旧 `build-stub.sh` / `install.sh` / 第二套 `google/` 或 `stub/` 源码删掉。
- 没人调用的函数、类型、头文件、脚本删掉。不要留 `#if 0`、注释掉的旧实现、「以后可能用」的拷贝。
- AIDL 接口上必须 override 的 `Bn*` 可以返回 `EX_UNSUPPORTED_OPERATION`，那是接口；接口以外多出来的 helper / 整份 Cuttlefish 默认实现不要带进来。
- 不要整目录复制 AOSP HAL。对照 `light`：能开机的最小实现。注释只写非显而易见的约束（冻结 AIDL hash、SELinux、QEMU 限制）。
- 改完自检：每个留下的 `.cpp/.h/.sh` 都在 `Android.bp` 或 `gki_custom` 的 `build.sh` 里用到；每个留下的函数都有调用点。
- **命名**：类型/类 `PascalCase`，函数/方法 `camelCase`，`#define` 常量 `SCREAMING_SNAKE`。不要 `do_stuff_impl2`、`utils_helper`、`tools_xxx`。
- **不要拆只有自己用的工具函数**。一段逻辑只有这一处调用，就写在该函数里，不要再抽 `util.cpp` / `tools.cpp` / `helpers.h`。第二个调用点出现了再拆。
- **常量**：魔法数字和路径字符串要么放在该 `.c/.cpp` **文件最上方** `#define`，要么新建一个 `.h` 给共用方 include。不要写在函数肚子里。

细则见 `.ai/AGENTS.md` 的 Code conventions。

## 实现要求

目标：framework 能在 VINTF 里找到实例并完成注册。对照开机日志（servicemanager / init 重启）和 FCM，选矩阵允许的冻结 AIDL 版本。最小 Bn*：boot 不需要的路径返回安全默认或 `EX_UNSUPPORTED_OPERATION`。KeyMint 必须有真加密，用 common 的 `security/keymint/.../soft`，不要空 stub。

编完装进 vendor，停 QEMU 后 `scripts/build-vendor-img.sh` + `scripts/build-initramfs.sh`（或 `./run`）。开机确认 `Found` / `registered`，并列出仍缺接口。不要做无关重构，不要擅自发布 git。

我点名的 HAL / 变体是：
