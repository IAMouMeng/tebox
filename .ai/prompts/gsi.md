先读根目录 README.md，再读 .ai/AGENTS.md 与当前 variant 的 FETCHED_FROM.txt，使用 gki-gsi 技能。
目标：把工程适配到我提供的新 system.img / 新 GSI 版本。
- 官方 aosp_arm64：更新 `.ci/guest.lock.json` / FETCHED_FROM，按 CI 摘要记录 BUILD_ID、fingerprint、SDK。
- 厂家 / MysticGSI（OneUI、ColorOS、OriginOS、Flyme 等）：再读 .ai/prompts/oem-adapt.md 与对应家族 prompt，不要只按 AOSP 路径做。
正确放置 system.img（sparse 需转换）；检查 KERNEL/GKI；按 servicemanager/VINTF 补 HAL（gki-hal）。
停 QEMU 后重打 vendor.img / initramfs，SNAPSHOT 试启动。
报告仍缺接口、内核是否需升级。不要擅自改 git 远程或发布仓库。
