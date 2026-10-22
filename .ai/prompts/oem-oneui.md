先读根目录 README.md、.ai/AGENTS.md，再读 .ai/prompts/oem-adapt.md。
使用 gki-gsi（+ 需要时 gki-hal）。金标对照：
src/aosp/Oneui-a25x-14-A256EXXS7BXK1-AB-20241130-MysticGSI/
（空壳 Oneui-pa3q / gts9wifi 仅作历史 donor 说明，树可能已空，勿当活参考。）

Samsung OneUI 适配要点：
1. 目录与镜像：手置 MysticGSI system.img；FETCHED_FROM 写清 OneUI 版本、机型、指纹；KERNEL 与 API 匹配（a25x 现用 gki-6.1-android14-gsi）。
2. GPU：该金标默认 virgl；若改 gfxstream，注意 A14 符号兼容（hardware/gfxstream-a14 一类 shim），并用 FORCE_* 单次验证。
3. 必做 Samsung stub：对 waitForService 死等的私有 AIDL（KnoxGuard、DisplayQuality、KHDM、SehKeyMint/Fkeymaster、SehWifi、SehHyPer/DVFS 假频表等）提供空实现 + VINTF，参考 hardware/samsung-stubs。
4. Radio：在 soft radio 上补 SemRIL / ISehRadio*（seh_radio），避免电话/IMS 相关卡死。
5. Audio：A14 OneUI 优先 HIDL 6.0 路径（hardware/audio-hidl），注意 VNDK.v34 的 LD_LIBRARY_PATH；勿想当然换成 AOSP AIDL audio。
6. KeyMint：用 soft 真实现；vendor 侧 bin + /vendor/lib64/keymint；兼容层可参考历史 Flyme A15 库说明；屏蔽冲突的 system KeyMint rc（initramfs bind）。
7. Props：ro.config.knox=v00；ro.sf.init.lcd_density=420（或等价）；ro.vndk.version / ro.product.first_api_level 与镜像一致；勿让 vendor fingerprint 与 system 打架。
8. Init：偏 initramfs bind 覆盖（platform.xml、vndk.rc、launcher URI grant、theme overlays、mask virtual_camera）；一般不 bake 进 system.img。
9. SELinux：bring-up 可用 permissive；plat_sepolicy_vers 跟镜像（如 34.0）。
10. 验证：SNAPSHOT 开机；确认 samsung-stubs / keymint / audio 已注册；列出仍缺的 Seh* 接口。

我的 OneUI 镜像与任务是：
