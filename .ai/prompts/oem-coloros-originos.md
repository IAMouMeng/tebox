先读根目录 README.md、.ai/AGENTS.md，再读 .ai/prompts/oem-adapt.md。
使用 gki-gsi（+ 需要时 gki-hal）。金标对照（ColorOS 系当前活树）：
src/aosp/Originos-PD2266-15-compiler250711173629-AB-20260208-MysticGSI/
Coloros-* 目录可能已空，仅作 FETCHED_FROM donor 记录；新 ColorOS 适配从 OriginOS 金标叉出再按日志改。

ColorOS / OriginOS（OPPO·一加·vivo）适配要点：
1. 镜像：手置 MysticGSI；FETCHED_FROM 写产品名（ColorOS/OriginOS）、机型、指纹、Android 版本。
2. hardware 身份：常按 **qcom** 而非 ranchu——cmdline `androidboot.hardware=qcom`、board.platform（如 kalama）、必要时 qemu/vulkan-hal=qcom（vulkan.ranchu.so → vulkan.qcom.so）。
3. Init 策略：无 /init.ranchu.rc 的 GSI 用 vendor 占位 + initramfs bind，或启用 qemu/init/bake-overlays → scripts/bake-system-overlays.sh 写进 system.img（改 product、去掉冲突 qemu props）。选一种主路径，写进 FETCHED_FROM。
4. Oplus / vivo 扩展 HAL：
   - charger：vendor.oplus.hardware.charger（版本跟 VINTF）
   - OriginOS：vivo_daemon.rc、vivo-algo（IVASAlgoProvider）解除相册/相机 wait；算法可空
5. Props 雷区：
   - 音量步长 ro.config.*_vol_steps / default，避免 SystemUI 除零
   - persist.sys.oplus.anim_level：避开已知崩溃档位（金标注释：zygote/boot 后强制安全值）
   - ro.mediaserver.64b.enable 等按 donor 保留到确认无害再删
6. SELinux：OriginOS 金标可为 enforcing；若卡权限先 permissive 定位再收紧，勿一上来当 AOSP 默认 permissive。
7. Radio：国内双卡可按金标 soft 运营商配置；无需求再砍。
8. KeyMint：soft + 兼容/attestation 路径跟 donor；sepolicy 版本跟镜像（如 202404）。
9. ld.config：若 logd/vold 找不到库，检查 /system_ext 搜索路径。
10. 验证：SNAPSHOT 开机；charger/vivo-algo/graphics/keymint 注册情况；anim/音量/SystemUI 是否秒崩。

我的 ColorOS 或 OriginOS 镜像与任务是：
