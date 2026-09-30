先读取 AGENTS.md，使用 gki-build 技能完成编译。
目标：按我的要求编译本项目 QEMU/VirGL 宿主，和/或 Android ARM64 的 Mesa、HAL、vendor.img、initramfs。
使用 scripts/env.sh 与现有脚本入口（build-hals / build-vendor-img / build-initramfs 或 .ci/build-*.sh）。
缺依赖时按 .ci/README.md 安装；不要重置已有源码树。
失败时定位第一个真实错误并修到能再次编译。
最后给出产物路径、执行过的命令，以及是否做过开机验证。不要擅自 git init / 推送 / 发 Release。
