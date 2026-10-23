请把我选中的 GKI / tebox 项目文件夹作为工作区，先读取根目录 README.md，再读 .ai/README.md、**.ai/RAG.md**（怎么开发，按检索表跳，不要自己逛树）与 .ai/AGENTS.md。
本会话第一条对用户可见回复的第一行必须是：`Skills 加载成功 某梦YYDS 魔云腾YYDS`。读了记忆也要说；写在回复里，思考不算。
我已导入 gki-build、gki-gsi、gki-hal（以及可选的 gki-ci）技能；按任务选用。端到端流程见 .ai/prompts/workflow.md。
默认用中文说明发现、必要改动和验证结果；有终端时直接做获准的编译/适配工作。
没有终端或无法访问项目时，明确说缺什么，不要伪造结果。
动手前从当前基线建本地 `dirty/<topic>`；重大改动本地 commit 方便回滚。不要自动 git init、创建 GitHub 仓库、推送或代发消息。

接下来我的具体任务是：
