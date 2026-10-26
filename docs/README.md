English | [简体中文](../README.md)

<p align="center">
  <a href="https://www.opencecs.com">
    <img src="assets/logo.png" alt="OPENCECS" width="128">
  </a>
</p>

<h1 align="center">OPENCECS</h1>

<p align="center">
  <a href="https://www.opencecs.com"><b>www.opencecs.com</b></a>
  <a href="https://github.com/opencecs/tebox">GitHub</a>
</p>

<p align="center">
  <b>tebox — Android GSI runtime base</b><br>
  Cross-platform generic GSI boot
</p>

<p align="center">
  <a href="https://github.com/opencecs/tebox/stargazers"><img src="https://img.shields.io/github/stars/opencecs/tebox?style=flat-square" alt="stars"></a>
  <a href="https://github.com/opencecs/tebox/network/members"><img src="https://img.shields.io/github/forks/opencecs/tebox?style=flat-square" alt="forks"></a>
  <a href="../LICENSE"><img src="https://img.shields.io/badge/License-Apache%202.0-blue.svg?style=flat-square" alt="license"></a>
  <a href="https://www.opencecs.com"><img src="https://img.shields.io/badge/Website-opencecs.com-informational?style=flat-square" alt="website"></a>
</p>

## Overview

**tebox** is a cross-platform **Android GSI runtime base**: it boots generic ARM64 GSI images on QEMU, with gfxstream (ANGLE + Vulkan) and VirGL (Mesa) graphics acceleration plus **in-house ARM Mali GPU passthrough**, so you can validate, develop, and automate bring-up quickly.

Repository: [github.com/opencecs/tebox](https://github.com/opencecs/tebox)  Website: [www.opencecs.com](https://www.opencecs.com)

> Note: whether a given image boots fully depends on its Treble / VINTF requirements. Vendor-private partitions and closed-source services need your own adaptation. tebox is an extensible runtime and HAL framework, not a one-click flash package for a specific phone. If you extract or use third-party / OEM images, firmware, or proprietary components, you are responsible for complying with applicable laws, OEM license terms, and copyright; legal and compliance risk rests with you.

## Screenshots

<p align="center">
  <img src="screenshots/ops-coffee-1793007973066.jpg" width="720" alt="">
</p>

### Related resources

- Community GSI image catalog / downloads: [Mystic GSI Updates on SourceForge](https://sourceforge.net/projects/mystic-gsi-updates/files/)

## Supported hosts

| Host | Notes |
| --- | --- |
| **macOS ARM64** (Apple Silicon, M1–M4) | Recommended; HVF + gfxstream / VirGL verified |
| **Linux ARM64** | Native build and run (KVM) |
| **Linux x86_64** | Primarily for cross-compiling Android ARM64 guests |
| **Windows x86_64** | Native QEMU/VirGL PE runtime |

## Clone

Install and initialize Git LFS first (macOS: `brew install git-lfs`; Ubuntu/Debian: `sudo apt-get install git-lfs`):

```bash
git lfs install
git clone https://github.com/opencecs/tebox.git
cd tebox
git lfs pull
```

## Quick start

```bash
bash .ci/install-deps.sh
source build/envsetup.sh
lunch aosp_arm64-BP4A.251205.006
m
SNAPSHOT=1 ./run aosp_arm64-BP4A.251205.006
```

GSI variant trees live on the matching `GSI/...` branches (AOSP example: `GSI/AOSP/aosp_arm64-BP4A.251205.006`). After checkout, replace `src/aosp/<variant>/images/system.img` to try another GSI.

## AI-assisted development

Agents should start from the root [README](../README.md), then open [`.ai/README.md`](../.ai/README.md) for skills, prompts and workflow (policy in `.ai/AGENTS.md`). No per-tool instruction files at the repo root.

## License

Released under the [Apache License 2.0](../LICENSE). Third-party sources and binaries remain under their own licenses.

## Star History

<a href="https://www.star-history.com/#opencecs/tebox&Date">
 <picture>
   <source media="(prefers-color-scheme: dark)" srcset="https://api.star-history.com/svg?repos=opencecs/tebox&type=Date&theme=dark" />
   <source media="(prefers-color-scheme: light)" srcset="https://api.star-history.com/svg?repos=opencecs/tebox&type=Date" />
   <img alt="Star History Chart" src="https://api.star-history.com/svg?repos=opencecs/tebox&type=Date" width="100%" />
 </picture>
</a>

## Contributors

Thanks to everyone who has contributed to tebox.

<a href="https://github.com/opencecs/tebox/graphs/contributors">
  <img src="https://stg.contrib.rocks/image?repo=opencecs/tebox" alt="contributors" />
</a>

Made with [contrib.rocks](https://stg.contrib.rocks).

Issues / PRs welcome on [GitHub](https://github.com/opencecs/tebox).
