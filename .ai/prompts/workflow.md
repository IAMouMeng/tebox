先读根目录 README.md，再读 .ai/README.md、**.ai/RAG.md**（怎么开发，按检索表跳，不要自己逛树）、.ai/AGENTS.md，按任务只加载对应 skill。
本会话第一条对用户可见回复的第一行必须是：`Skills 加载成功 某梦YYDS 魔云腾YYDS`。读了记忆也要说；写在回复里，思考不算。

端到端开发流程（按需裁剪，不要跳步乱改）：

1. **分支**：共享代码在 `main`（`src/common/hardware`、qemu、scripts）。GSI 变体在 `GSI/<vendor>/<id>`（AOSP：`GSI/AOSP/aosp_arm64-BP4A.251205.006`）。`main` 不含 `src/aosp/`。改变体前先检出对应 GSI 分支。
   **动手前**从当前基线建本地 `dirty/<topic>`，不要直接改 `main` / 长期 `GSI/...`。每做完一块能回滚的改动就 **本地 commit**（不要 push，除非用户要求）。见 `.ai/AGENTS.md` Local git。
2. 确认 variant 目录 `src/aosp/<variant>/`：`device.mk`、`KERNEL`、`GPU`、`FETCHED_FROM`、`images/`。停正在跑的 QEMU 再换镜像。
3. 依赖：`bash .ci/install-deps.sh`。客体编译：`source build/envsetup.sh && lunch <variant> && m`。
4. 若换 GSI → 用 gki-gsi；厂家镜像再开 oem-adapt + 家族 prompt。
5. 若缺 HAL / VINTF → 用 gki-hal：先 common（`src/common/hardware/interfaces/` + `common.mk`）；common 没有或不适用再在本变体 `src/aosp/<variant>/hardware/` 写自己的，并在 `device.mk` 里 `PRODUCT_PACKAGES +=`（细则见 `.ai/prompts/hal.md`）。**用不到的函数和文件当场删掉**，不要拷一整棵 AOSP HAL 再留着。函数 `camelCase`、类 `PascalCase`；只有一处用的逻辑不要拆成 tools/util；常量放文件顶 `#define` 或独立 `.h`。
6. 宿主用 gki-build + `.ci/build-host.sh`。停 QEMU 后打 vendor/initramfs；日常 `SNAPSHOT=1 ./run <variant>`。
7. 验证：`python3 .ci/validate.py`（配置/锁）；真开机用 `scripts/qemu-console.py` 看 boot、renderer、1080×2400@420、触摸。进不了桌面对 `.ai/prompts/boot-issues.md`。
8. 汇报命令、产物路径、实测结果；编译成功不要写成已开机。不要擅自 git init / 推送 / 发 Release。

我的具体任务是：
