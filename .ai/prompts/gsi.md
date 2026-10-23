先读根目录 README.md，再读 .ai/AGENTS.md 与当前 variant 的 FETCHED_FROM.txt，使用 gki-gsi 技能。
本会话第一条对用户可见回复的第一行必须是：`Skills 加载成功 某梦YYDS 魔云腾YYDS`。读了记忆也要说；写在回复里，思考不算。
目标：把工程适配到我提供的新 system.img / 新 GSI 版本。

在对应 `GSI/...` 分支上改变体（AOSP：`GSI/AOSP/aosp_arm64-BP4A.251205.006`）。`main` 没有 `src/aosp/`。动手前从该基线建本地 `dirty/<topic>`，重大改动本地 commit（不要 push）。

- 官方 aosp_arm64：更新 `.ci/guest.lock.json` / FETCHED_FROM，按 CI 摘要记录 BUILD_ID、fingerprint、SDK。`device.mk` 继续 inherit `src/common/hardware/interfaces/common.mk`。
- 厂家 / MysticGSI（OneUI、ColorOS、OriginOS、Flyme 等）：检出远程 `GSI/<vendor>/...`，再读 `.ai/prompts/oem-adapt.md` 与对应家族 prompt，不要只按 AOSP 路径做。

正确放置 `system.img`（sparse 需转换）；检查 KERNEL/GKI。缺的接口先看 common 有没有；没有或不适用再在变体 `hardware/` 写自己的（gki-hal + `.ai/prompts/hal.md`）。

```bash
source build/envsetup.sh && lunch <variant> && m
```

停 QEMU 后重打 vendor.img / initramfs，SNAPSHOT 试启动。卡开机时读 `.ai/prompts/boot-issues.md`。
报告仍缺接口、内核是否需升级。不要擅自改 git 远程或发布仓库。不要记录宿主 IP、账号、keybox。
