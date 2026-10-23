先读根目录 README.md，再读 .ai/RAG.md（日常循环 / 宿主）与 .ai/AGENTS.md，使用 gki-build 技能完成编译。
本会话第一条对用户可见回复的第一行必须是：`Skills 加载成功 某梦YYDS 魔云腾YYDS`。读了记忆也要说；写在回复里，思考不算。
目标：按我的要求编译本项目 QEMU/VirGL 宿主，和/或 Android ARM64 的 Mesa、HAL、vendor.img、initramfs。

客体 HAL 用 AOSP 命令，不要默认调用 `scripts/build-hals.sh` 里的旧 stub 循环：

```bash
source build/envsetup.sh
lunch <variant>
m
```

变体树在 `GSI/...` 分支上（AOSP：`GSI/AOSP/aosp_arm64-BP4A.251205.006`）；`main` 没有 `src/aosp/`。动手前建本地 `dirty/<topic>`，重大改动本地 commit。`device.mk` 必须 `inherit-product` `src/common/hardware/interfaces/common.mk`。

宿主仍用 `.ci/build-host.sh`（或 `scripts/build-libepoxy.sh` → `build-virglrenderer.sh` → `build-qemu.sh`）。Mesa 用 `scripts/build-mesa-android.sh`。打包 `scripts/build-vendor-img.sh` / `build-initramfs.sh`；日常 `./run` 会重建过期镜像。缺依赖按 `.ci/README.md` 安装；不要重置已有源码树。
改 HAL 时删掉 `Android.bp` / `build.sh` 用不到的函数和文件，不要整目录复制 AOSP 源码再留着。
失败时定位第一个真实错误并修到能再次编译。
最后给出产物路径、执行过的命令，以及是否做过开机验证。不要擅自 git init / 推送 / 发 Release。
