# AI tools: skills and prompts

Canonical skill bodies live in `.ai/skills/*/SKILL.md`. Prompts live in
`.ai/prompts/`. Project policy is `AGENTS.md` (read it first).

## Skills

| Skill | Use when |
| --- | --- |
| `gki-build` | Compile host QEMU/VirGL or Android Mesa/HAL/vendor/initramfs |
| `gki-gsi` | Adapt the tree to a **new `system.img` / GSI** build |
| `gki-hal` | Implement or fix a **soft vendor HAL** (AIDL + VINTF + pack) |
| `gki-ci` | Maintain `.gitignore`, locks, GitHub Actions, artifacts |

```bash
python3 .ci/link-ai-skills.py
python3 .ci/package-skills.py   # → dist/skills/*.zip
```

## Per-tool wiring

| Tool | Guidance |
| --- | --- |
| **Cursor** | `AGENTS.md` + `.cursor/rules/gki.mdc` + `.cursor/skills/` → `.ai/skills/` |
| **Codex** | `AGENTS.md` + `.agents/skills/` → `.ai/skills/` |
| **Claude Code** | `CLAUDE.md` + `.claude/skills/` → `.ai/skills/` |
| **Gemini CLI** | `GEMINI.md` + `.agents/skills/` → `.ai/skills/` |
| **WorkBuddy** | Import `dist/skills/*.zip`, paste `.ai/prompts/workbuddy.md` + task |

## Prompt templates

| File | Intent |
| --- | --- |
| `.ai/prompts/build.md` | 编译宿主或客体 |
| `.ai/prompts/gsi.md` | 适配新 system.img |
| `.ai/prompts/hal.md` | 开发 / 修补 HAL |
| `.ai/prompts/ci.md` | 维护 CI / lock / 产物 |
| `.ai/prompts/workbuddy.md` | WorkBuddy 开场白 |

## Rules

- Stay in the user’s scope; no surprise `git init` / push / publish.
- Compile ≠ boot proof. Say what was actually tested.
- Prefer existing HAL stub patterns under `src/aosp/<variant>/hardware/`.
