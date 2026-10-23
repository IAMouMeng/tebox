# AI tools: skills, prompts, workflow

根目录只保留 `README.md` 作为人类与 Agent 的统一入口。Agent 读完 README 后到本目录
自取约束、技能与提示词；不要在仓库根再堆各厂商的 `CLAUDE.md` / `GEMINI.md` /
`AGENTS.md`。

| Path | Role |
| --- | --- |
| [AGENTS.md](AGENTS.md) | 源码边界、GPU/开机不变量、验证与汇报规则（必读） |
| `skills/*/SKILL.md` | 可执行技能正文（唯一权威副本） |
| `prompts/*.md` | 可粘贴的任务开场白 |

```bash
python3 .ci/link-ai-skills.py   # 链到 .agents / .claude / .cursor
python3 .ci/package-skills.py   # → dist/skills/*.zip
```

## 开发流程（Agent 按任务自选）

1. **读入口**：根 `README.md` → 本文件 → `AGENTS.md`；涉及 CI 时再读 `.ci/README.md`。
2. **定任务类型**，只加载对应 skill + prompt（不要一次读全）：

| 你在做什么 | Skill | Prompt |
| --- | --- | --- |
| 编译宿主 QEMU/VirGL 或客体 Mesa/HAL/vendor/initramfs | `gki-build` | [prompts/build.md](prompts/build.md) |
| 换 / 升官方 `system.img`（AOSP GSI） | `gki-gsi` | [prompts/gsi.md](prompts/gsi.md) |
| **厂家 GSI 适配**（OneUI / ColorOS / OriginOS / Flyme） | `gki-gsi` (+ `gki-hal`) | [prompts/oem-adapt.md](prompts/oem-adapt.md) → 家族 prompt |
|  ↳ Samsung OneUI | 同上 | [prompts/oem-oneui.md](prompts/oem-oneui.md) |
|  ↳ ColorOS / OriginOS | 同上 | [prompts/oem-coloros-originos.md](prompts/oem-coloros-originos.md) |
|  ↳ Flyme（参考树可能为空） | 同上 | [prompts/oem-flyme.md](prompts/oem-flyme.md) |
| 写或修 soft vendor HAL | `gki-hal` | [prompts/hal.md](prompts/hal.md) |
| 维护 ignore / lock / Actions / 产物 | `gki-ci` | [prompts/ci.md](prompts/ci.md) |
| 不确定从哪下手 / 端到端 bring-up | （按上表组合） | [prompts/workflow.md](prompts/workflow.md) |
| WorkBuddy 导入 zip 后开场 | 已导入的 skills | [prompts/workbuddy.md](prompts/workbuddy.md) |

3. **改代码前**：确认 variant（`src/aosp/<variant>/`）、`KERNEL` / `GPU`、是否在跑 QEMU；停 VM 再换镜像。
4. **构建入口**：`source build/envsetup.sh && lunch && m`；宿主 `.ci/build-host.sh`；日常启动 `./run`（会重建过期的 `vendor.img` / `initramfs.img`）。
5. **验证**：配置/锁变更跑 `python3 .ci/validate.py`。编译成功 ≠ 已开机；真机验证用 `FORCE_VIRGL=1`（或 variant 的 `GPU`）+ `SNAPSHOT=1`，并用 `scripts/qemu-console.py` 看 boot / renderer / 尺寸密度。
6. **汇报**：写清执行过的命令、产物路径、实际测到什么；未测平台写明未测。不擅自 `git init` / push / 发 Release。

## Skills（摘要）

| Skill | Use when |
| --- | --- |
| `gki-build` | Compile host QEMU/VirGL or Android Mesa/HAL/vendor/initramfs |
| `gki-gsi` | Adapt the tree to a **new `system.img` / GSI** build |
| `gki-hal` | Implement or fix a **soft vendor HAL** (AIDL + VINTF + pack) |
| `gki-ci` | Maintain `.gitignore`, locks, GitHub Actions, artifacts |

## Per-tool wiring

各工具只链 skills；工程说明一律走 README → `.ai/`。

| Tool | Guidance |
| --- | --- |
| **Cursor** | 根 `README.md` + `.cursor/rules/gki.mdc` + `.cursor/skills/` → `.ai/skills/` |
| **Codex** | 根 `README.md` + `.agents/skills/` → `.ai/skills/`（策略在 `.ai/AGENTS.md`） |
| **Claude Code** | 根 `README.md`；可选 `.claude/CLAUDE.md` 指针 + `.claude/skills/` → `.ai/skills/` |
| **Gemini CLI** | 根 `README.md` + `.agents/skills/` → `.ai/skills/` |
| **WorkBuddy** | Import `dist/skills/*.zip`，粘贴 `prompts/workbuddy.md` + 任务 |

## Rules（短）

- Stay in the user’s scope; no surprise `git init` / push / publish.
- Compile ≠ boot proof. Say what was actually tested.
- Prefer existing HAL patterns under `src/common/hardware/interfaces/` (`Android.bp`) and variant `device.mk`.
