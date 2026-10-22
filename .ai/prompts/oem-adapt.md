先读根目录 README.md，再读 .ai/AGENTS.md、.ai/README.md，以及目标 variant 的 FETCHED_FROM.txt / KERNEL / GPU。
通用换镜像用 gki-gsi；本任务是**厂家 / OEM GSI 适配**（OneUI、ColorOS、OriginOS、Flyme 等），按家族再打开对应 prompt：
- OneUI → .ai/prompts/oem-oneui.md
- ColorOS / OriginOS（OPPO/一加/vivo 系）→ .ai/prompts/oem-coloros-originos.md
- Flyme → .ai/prompts/oem-flyme.md
缺 HAL 时再用 gki-hal。参考已装满的金标树，不要从空壳目录硬猜：
- AOSP 基线：src/aosp/aosp_arm64-BP4A.251205.006/
- OneUI：src/aosp/Oneui-a25x-14-A256EXXS7BXK1-AB-20241130-MysticGSI/
- OriginOS（ColorOS 系活树）：src/aosp/Originos-PD2266-15-compiler250711173629-AB-20260208-MysticGSI/

总流程（按需裁剪）：
1. 建或选定 src/aosp/<Variant>/：FETCHED_FROM、KERNEL、GPU、hardware/、qemu/、images/、prebuilts/gsi-lib64/。
2. 手置 system.img（MysticGSI / 厂商包通常无 guest.lock 下载）；记 sha256；sparse 先 simg2img；跑 extract-gsi-libs.sh。
3. 从最近似 donor 复制 vendor/HAL/qemu（在 FETCHED_FROM 写清来源），再按开机日志裁剪，禁止无关大重构。
4. Soft HAL 基线对齐 AOSP（graphics/keymint soft/health/power/audio…）；OEM 私有 waitForService 接口再 stub。
5. 按家族改 cmdline / init 覆盖 / bake-overlays / props（密度、VNDK、Knox、音量、hardware=ranchu|qcom 等）。
6. 停 QEMU → build-hals → build-vendor-img → build-initramfs（或 ./run 自动重建）。
7. SNAPSHOT=1 ./run <variant>；用 qemu-console.py 看 boot、servicemanager、renderer。编译成功 ≠ 已开机。
合规：第三方/厂商镜像与闭源组件由使用者自行确认许可；不要擅自 git 远程/发 Release。

我的厂家与具体任务是：
