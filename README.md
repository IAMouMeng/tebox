[English](docs/README.md) | 简体中文

<p align="center">
  <a href="https://opencecs.com">
    <img src="docs/assets/logo.png" alt="OPENCECS" width="128">
  </a>
</p>

<h1 align="center">OPENCECS</h1>

<p align="center">
  <a href="https://www.opencecs.com"><b>www.opencecs.com</b></a>
  <a href="https://github.com/opencecs/tebox">GitHub</a>
</p>

<p align="center">
  <b>tebox — 桌面端 Android GSI 运行基座</b><br>
  通用 GSI 镜像启动能力  VirGL 图形加速  Soft Vendor HAL 示例
</p>

<p align="center">
  <a href="https://github.com/opencecs/tebox/stargazers"><img src="https://img.shields.io/github/stars/opencecs/tebox?style=flat-square" alt="stars"></a>
  <a href="https://github.com/opencecs/tebox/network/members"><img src="https://img.shields.io/github/forks/opencecs/tebox?style=flat-square" alt="forks"></a>
  <a href="LICENSE"><img src="https://img.shields.io/badge/License-Apache%202.0-blue.svg?style=flat-square" alt="license"></a>
  <a href="https://opencecs.com"><img src="https://img.shields.io/badge/Website-opencecs.com-informational?style=flat-square" alt="website"></a>
</p>

## 项目简介

**tebox** 是一套面向桌面端的 **Android GSI 运行基座**：在 QEMU 上提供通用的 ARM64 GSI 镜像启动能力，并内置 VirGL / Mesa 图形加速、GKI 内核与 soft vendor HAL 示例，方便你直接上手验证、二次开发和自动化。

仓库：[github.com/opencecs/tebox](https://github.com/opencecs/tebox)  官网：[opencecs.com](https://opencecs.com)

### 基座能力

- **通用 GSI 运行**：以官方 aosp_arm64 GSI 为默认示例，也可替换为任意兼容的 ARM64 GSI（例如 Google、Samsung 等厂商发布的 GSI / Treble 镜像，或由 Pixel、Galaxy 等设备提取的 system 镜像）
- **图形与输入**：VirGL GPU 加速、virtio 触摸屏
- **开机所需 HAL 示例**：已内置 Graphics、KeyMint（soft）、Health、Power、Audio 等 soft / stub 实现，可按厂商需求裁剪或替换

> 说明：能否完整启动取决于目标 GSI 的 Treble / VINTF 要求；厂商专有分区与闭源服务需自行适配。基座提供的是可扩展的运行与 HAL 框架，而不是某一机型的一键刷机包。从设备提取或使用第三方 / 厂商镜像、固件与闭源组件时，请自行确认并遵守当地法律、厂商许可协议与版权要求；相关合规与法律风险由使用者自行承担。

### 相关资源

- 各类社区 GSI 镜像汇总下载：[Mystic GSI Updates（SourceForge）](https://sourceforge.net/projects/mystic-gsi-updates/files/)

## 适用场景

- 在桌面环境验证 / 对比不同 GSI 镜像
- 开发与调试 vendor HAL、图形栈与系统服务
- 搭建 CI，对 GSI + HAL 改动做自动化编译与冒烟

## 运行环境

| 宿主 | 说明 |
| --- | --- |
| **macOS ARM64**（Apple Silicon，M1–M4） | 推荐；HVF + VirGL 已验证 |
| **Linux ARM64** | 支持原生构建与运行（KVM） |
| **Linux x86_64** | 主要用于交叉编译 Android ARM64 客体 |

客体为 **aarch64 Android GSI**；推荐配备可用 GPU 与图形会话。

## 启动截图

<p align="center">
  <img src="docs/screenshots/launched.png" width="240" alt="tebox 启动界面">
</p>

## 正确拉取源码

本仓库使用 **Git LFS** 保存 `system.img`、GKI 内核 `Image` 和内核模块 `*.ko`。请先安装 Git 和 Git LFS：macOS 可执行 `brew install git-lfs`；Ubuntu / Debian 可执行 `sudo apt-get install git-lfs`。

```bash
git lfs install
git clone https://github.com/opencecs/tebox.git
cd tebox
git lfs pull
```

如果已经克隆过仓库，在仓库根目录执行以下命令即可补齐大文件：

```bash
git lfs install
git lfs pull
```

`system.img` 应为 GB 级镜像。如果文件只有一百多字节，内容以 `version https://git-lfs.github.com/spec/v1` 开头，说明拿到的是 LFS 指针，尚未下载真实文件；内核和模块也可能处于同样状态，无法用于启动。请完成 `git lfs pull` 后再运行。发布仓库时，维护者也必须上传对应的 LFS 对象；仅推送指针文件无法让其他人取得完整镜像。

`vendor.img` 和 `initramfs.img` 不提交到仓库，由 `./run` 根据源码自动生成。首次使用还需准备当前宿主平台的 QEMU/VirGL，构建入口见 [构建说明](.ci/README.md)；`out/` 中的本地编译结果不会随克隆下载。

## 快速开始

```bash
bash .ci/install-deps.sh
FORCE_VIRGL=1 SNAPSHOT=1 ./run
```

默认会启动仓库内置的 AOSP GSI 示例变体。替换 `src/aosp/<variant>/images/system.img` 即可尝试其他 GSI。

## 许可证

本项目以 [Apache License 2.0](LICENSE) 开源。第三方源码与二进制仍遵循其各自许可证。

## Star History

<a href="https://www.star-history.com/#opencecs/tebox&Date">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=opencecs/tebox&type=Date&theme=dark" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=opencecs/tebox&type=Date" />
   <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=opencecs/tebox&type=Date" width="100%" />
 </picture>
</a>

## 贡献者

感谢所有参与 tebox 的贡献者。

<a href="https://github.com/opencecs/tebox/graphs/contributors">
  <img src="https://contrib.rocks/image?repo=opencecs/tebox" alt="contributors" />
</a>

Made with [contrib.rocks](https://contrib.rocks).

欢迎在 [GitHub](https://github.com/opencecs/tebox) 提交 Issue / PR。
