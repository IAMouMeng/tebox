先读根目录 README.md、.ai/AGENTS.md，再读 .ai/prompts/oem-adapt.md。
使用 gki-gsi（+ 需要时 gki-hal）。检出远程 `GSI/OneUI/<id>`；`device.mk` inherit common。Samsung 私有 stub 写在该变体 `src/aosp/<variant>/hardware/`，不要写进 `src/common`。

Samsung OneUI 适配要点：
1. 目录与镜像：手置 MysticGSI system.img；FETCHED_FROM 写清 OneUI 版本、机型、指纹；KERNEL 与 API 匹配。
2. GPU：按该变体 `GPU` 文件；若改 gfxstream，注意 A14 符号兼容，并用 FORCE_* 单次验证。
3. 必做 Samsung stub：对 waitForService 死等的私有 AIDL（KnoxGuard、DisplayQuality、KHDM、SehKeyMint/Fkeymaster、SehWifi、SehHyPer/DVFS 假频表等）在变体 `hardware/` 提供空实现 + VINTF，并 `PRODUCT_PACKAGES +=`。
4. Radio：在 soft radio 上补 SemRIL / ISehRadio*（seh_radio），避免电话/IMS 相关卡死。
5. Audio：A14 OneUI 优先 HIDL 6.0 路径（变体 `hardware/` 覆盖 common AIDL audio 如需要），注意 VNDK.v34 的 LD_LIBRARY_PATH。
6. KeyMint：用 common 的 keymint soft 真实现；vendor 侧 bin + /vendor/lib64/keymint；屏蔽冲突的 system KeyMint rc（initramfs bind）。
7. Props：ro.config.knox=v00；ro.sf.init.lcd_density=420（或等价）；ro.vndk.version / ro.product.first_api_level 与镜像一致；勿让 vendor fingerprint 与 system 打架。
8. Init：偏 initramfs bind 覆盖（platform.xml、vndk.rc、launcher URI grant、theme overlays、mask virtual_camera）；一般不 bake 进 system.img。
9. SELinux：bring-up 可用 permissive；plat_sepolicy_vers 跟镜像（如 34.0）。
10. 验证：SNAPSHOT 开机；确认 samsung-stubs / keymint / audio 已注册；列出仍缺的 Seh* 接口。

我的 OneUI 镜像与任务是：
