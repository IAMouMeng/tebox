先读取 AGENTS.md 和 .ci/README.md，使用 gki-ci 技能维护本项目 CI。
目标是 macOS ARM64、Linux ARM64 的原生 QEMU/VirGL 构建，以及 Android ARM64 客体的交叉编译。
从没有 out、prebuilts、SDK/NDK 链接、嵌套 Git 工作树的干净 checkout 检查依赖是否齐全。
保证下载校验、平台依赖、产物打包和失败日志相互匹配。
保持只读仓库权限，不上传用户数据或密钥，不创建远程仓库或发布 Release。
运行本地可执行的验证，区分本地测试结果与尚未执行的 GitHub Actions 结果。
