先读根目录 README.md，再读 .ai/README.md 与 .ai/AGENTS.md，按任务只加载对应 skill。

端到端开发流程（按需裁剪，不要跳步乱改）：

1. 确认宿主平台与目标 variant（src/aosp/<variant>/：KERNEL、GPU、FETCHED_FROM、images/）。
2. 依赖：bash .ci/install-deps.sh；路径用 source scripts/env.sh。
3. 若换 GSI → 用 gki-gsi；厂家镜像再开 oem-adapt + 家族 prompt（oem-oneui / oem-coloros-originos / oem-flyme）。
4. 若缺 HAL / VINTF → 用 gki-hal（先对照同家族金标树，再对照 AOSP soft stub）。
5. 编译：宿主用 gki-build + .ci/build-host.sh；客体 Mesa/HAL/vendor/initramfs 用现有 scripts 或 .ci/build-android.sh。
6. 停 QEMU 后打包镜像；日常用 SNAPSHOT=1 ./run（GPU 按 variant 的 GPU 文件，或 FORCE_VIRGL=1 / FORCE_RUTABAGA）。
7. 验证：python3 .ci/validate.py（配置/锁）；真开机用 scripts/qemu-console.py 看 boot、renderer、1080×2400@420、触摸。
8. 汇报命令、产物路径、实测结果；编译成功不要写成已开机。不要擅自 git init / 推送 / 发 Release。

我的具体任务是：
