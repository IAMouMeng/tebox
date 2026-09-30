先读取 AGENTS.md 与当前 variant 的 FETCHED_FROM.txt，使用 gki-gsi 技能。
目标：把工程适配到我提供的新 system.img / 新 GSI 版本。
记录新 BUILD_ID、fingerprint、SDK；更新 `.ci/guest.lock.json` / FETCHED_FROM；正确放置 system.img（sparse 需转换）。
检查 KERNEL/GKI 是否仍匹配；根据 servicemanager/VINTF 缺失项补 HAL（需要时再用 gki-hal）。
停 QEMU 后重打 vendor.img / initramfs，用 SNAPSHOT 试启动。
报告还能缺哪些接口、内核是否需要升级。不要擅自改 git 远程或发布仓库。
