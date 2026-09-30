---
name: gki-ci
description: Maintain the project's Git ignore rules, pinned sources, ARM64 GitHub Actions builds and downloadable build artifacts.
---

Locate the project and read `AGENTS.md`, `.ci/README.md` and `.github/workflows/build.yml`.
Keep the current build goal distinct from repository publication: authoring a
workflow does not authorize creating a remote repository or uploading local data.

Work from clean-checkout requirements. `qemu/`, `thirdparty/`, `prebuilts/`
(host + android-arm64), `toolchains/`, each variant’s `images/` (`system.img`,
`gsi-lib64/`), `vendor`, KeyMint `keybox.xml` and GKI binaries are tracked
(Git LFS for large files). Only `out/`, `dist/` and `downloads/` are disposable
build/cache products.

Keep native host builds on `macos-14` and `ubuntu-24.04-arm`, with an architecture
assertion in the build script. Android outputs are ARM64, built using official
NDK tools on the Linux x86_64 job. Extra download checksums belong in
`.ci/guest.lock.json`, not in ad hoc runner commands.

Keep workflow permissions at `contents: read` for compile/upload-artifact work.
Use PR workflows without secret-dependent build steps. Upload only selected
runtime packages, project skill ZIPs and relevant build logs; never upload the
whole workspace or userdata images from `out/`.

Run `.ci/validate.py` and the affected build/package scripts. Check architecture
and that packaged host binaries report version / expected devices. Record local
validation separately from an actual GitHub Actions run.
