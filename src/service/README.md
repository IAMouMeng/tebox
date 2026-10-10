# `src/service` — host-side service tools

This tree holds the desktop **service** that will manage the emulator lifecycle
(create / delete / start / stop) and optional features such as **host cast**.

| Path | Role |
|------|------|
| [`cast/`](cast/) | Host-side screen cast (QEMU scanout → encode → TCP). Guest-unaware. |
| [`client/`](client/) | [Wails](https://wails.io) desktop launcher: create/delete Android instances, start QEMU and send Home/Back/volume/power controls over QMP. |

Guest Android MediaCodec / VirGL video is **not** this path; see
`hardware/codec2` and `thirdparty/virglrenderer` for that.

## Build

```bash
# Once: build bundled FFmpeg from thirdparty/ffmpeg
./scripts/build-ffmpeg-cast.sh

# Client (needs Go + Node + `go install github.com/wailsapp/wails/v2/cmd/wails@latest`)
# and cast binaries → out/service/
make -C src/service

# Launch the desktop emulator manager (instances live in ~/.tebox by default)
TEBOX_ROOT="$PWD" ./out/service/tebox-client

The client sets `TEBOX_WINDOW_CONTROLS=1` for launched instances. QEMU then
opens a small native side toolbar beside its display (volume up/down, Back,
Home and Power). The toolbar sends Linux input key events inside QEMU, so it
does not add another video or cast path. QEMU's SDL OpenGL surface also opts
into HiDPI drawable pixels; on Retina hosts this keeps Android text sharp while
preserving the guest's 1080×2400 mode.
```

Artifacts use only the project FFmpeg under `out/cast-ffmpeg` /
`prebuilts/host/<os-arch>/ffmpeg` (no system Homebrew `ffmpeg` required).

## Cast quick start

```bash
./out/service/tebox-cast --capture-sock "$SOCK" --codec h264 --listen 127.0.0.1:9901 &
TEBOX_CAST=1 TEBOX_CAST_SOCK="$SOCK" TEBOX_ROOT="$PWD" ./run
TEBOX_ROOT="$PWD" ./out/service/tebox-cast-view --port 9901
```
