# 工具链入口

`toolchains/` 随仓库保留。可将本机 NDK/SDK 链到这里，或设置环境变量：

```sh
ln -sfn /path/to/ndk toolchains/android-ndk
ln -sfn /path/to/sdk toolchains/android-sdk
# 或：
ANDROID_NDK=/path/to/ndk ANDROID_SDK_ROOT=/path/to/sdk bash .ci/build-mesa-android.sh
```

当前使用 NDK 30、SDK build-tools 36.1.0、Android API 34。macOS ARM 上，NDK 的工具子目录仍叫 `darwin-x86_64`。

CI 通过 `sdkmanager` 安装锁定版本的 NDK/build-tools；本地链接仅作开发便利。

HAL 编译也支持 `NDK`、`AIDL` 覆盖。公共配置在 `scripts/env.sh`。

宿主还需要 Xcode Command Line Tools（macOS）、meson、ninja、pkg-config、SDL2、libslirp、Mesa、e2fsprogs，以及 Mesa 所需的 Python 模块（如 Mako）。系统级工具保持在系统安装位置；编译产物在 `out/` / `prebuilts/`。
