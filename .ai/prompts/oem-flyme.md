先读根目录 README.md、.ai/AGENTS.md，再读 .ai/prompts/oem-adapt.md。
使用 gki-gsi（+ 需要时 gki-hal）。

Flyme 现状：src/aosp/Flyme-* 可能只剩 images/ 空壳；历史上曾向 OneUI A14 提供 KeyMint/binder 兼容库，**没有完整活参考树**。不要虚构 Meizu 私有 HAL。

建议路径：
1. 手置 Flyme system.img，写 FETCHED_FROM（机型、Android 版本、指纹、sha256）。
2. Donor 优先级：若有本机归档的旧 Flyme vendor/hardware 先恢复；否则从 AOSP 金标复制 soft HAL 基线，再按开机日志补缺。
3. KeyMint：优先核对 binder_ndk / keymint 符号是否与 A15+ 兼容；可对照 OneUI 金标里“来自 Flyme 的 compat”注释，而不是抄 OneUI 的 samsung-stubs。
4. 扫 servicemanager / init 重启：只为真实 wait 死的 Meizu/Flyme 接口加最小 stub（布局学 gki-hal + AOSP hardware/*）。
5. Props/cmdline：先 ranchu+permissive bring-up；确认无 qcom/oplus 依赖后再考虑改 hardware 名。
6. 验证与汇报：写清哪些来自恢复归档、哪些是新建 stub、仍缺哪些接口。无 donor 时明确说无法保证一次性开机。

我的 Flyme 镜像与任务是：
