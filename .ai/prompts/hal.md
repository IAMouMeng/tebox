先读取 AGENTS.md，使用 gki-hal 技能开发或修补 vendor HAL。
目标：实现/补齐我点名的 AIDL HAL，使 framework 能在 VINTF 里找到实例并完成注册。
对照现有 health/graphics/audio/power/keymint soft 的 stub 布局：源码、init rc、vintf xml、manifest、file_contexts、build-stub/install。
用冻结 AIDL 版本交叉编译，装进 vendor，再 build-vendor-img + build-initramfs。
开机确认 servicemanager 已 Found/registered，并说明仍缺失的接口。不要做无关重构，不要擅自发布 git 仓库。
