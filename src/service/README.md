# `src/service` — host-side service tools

This tree holds the desktop **service** that will manage the emulator lifecycle
(create / delete / start / stop) and optional features such as **host cast**.

| Path | Role |
|------|------|
| [`cast/`](cast/) | Host-side screen cast (QEMU scanout → encode → TCP). Guest-unaware. |
| [`client/`](client/) | [Wails](https://wails.io) desktop launcher: create/delete Android instances, start QEMU and send Home/Back/volume/power controls over QMP. |
| [`api/`](api/) | Host gRPC daemon for creating, deleting, starting, stopping and resetting Android instances. |

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
```

QEMU's SDL OpenGL surface opts into HiDPI drawable pixels; on Retina hosts this
keeps Android text sharp while preserving the guest's 1080×2400 mode. Navigation
keys (Home, Back, volume, power) come from `SendKey` or the cast viewer below.

Start the API process separately with `TEBOX_ROOT="$PWD" ./out/service/tebox-api`.
The Wails client connects to `TEBOX_API_ADDR` (default `127.0.0.1:50051`).
The gRPC API is defined in [`api/api/v1/tebox.proto`](api/api/v1/tebox.proto).
The API also exposes one cast endpoint (default `127.0.0.1:9901`). A cast
client selects the Android instance immediately after connecting by sending
`TEBOX-CAST/1 <instance-id>\n`; the remaining bytes use the existing encoded
cast stream format from [`cast/include/cast/protocol.h`](cast/include/cast/protocol.h).
Set `TEBOX_CAST_ADDR` or `--cast-listen` to change that endpoint.
`Instance.cast_port` reports the per-instance `tebox-cast` port while it runs.

Guest input (touch and keys) has three entry points, all feeding one
dedicated QMP monitor per instance (`qmp-input.sock`, from `QMP_INPUT_SOCKET`),
so input never holds the lifecycle QMP socket:

- `Instance.control_port`: plain TCP on 127.0.0.1. The server greets with
  `{"ok":true}`, then reads one JSON object per line and answers only on
  errors (`{"ok":false,"error":...}`).
- `TEBOX-CTRL/1 <instance-id>\n` on the cast endpoint, then the same lines.
- gRPC `SendInput` (one event) and `StreamInput` (client stream for gestures).

One JSON object per line:

| Event | Example | Path |
|------|---------|------|
| touch | `{"type":"touch","action":"down","x":0.5,"y":0.25}` (`down`/`move`/`up`, 0..1 from top-left) | QEMU virtio touchscreen |
| key | `{"type":"key","key":"home"}` (`home`, `back`, `menu`, `power`, `volume_up`, `volume_down`, `mute`, or a QEMU qcode) | QEMU virtio keyboard |
| text | `{"type":"text","text":"hi 你好"}` (ASCII is typed; other text is pasted) | guest agent |
| keycode | `{"type":"keycode","keycode":67,"meta":0,"action":"press"}` (Android `KEYCODE_*`/`META_*`; `down`/`up`/`press`) | guest agent |
| clipboard | `{"type":"clipboard","text":"...","paste":false}` | guest agent |

Guest clipboard changes come back on the same connection as
`{"type":"clipboard","text":"..."}` (gRPC: `WatchClipboard`); text set from the
host is not echoed back. The guest agent is the unmodified scrcpy-server in
`prebuilts/android-arm64/scrcpy/`, which tebox-api pushes and starts over adb on
first use (video and audio off; only its control socket is used). Touch and
navigation keys keep working without it, also before Android finishes booting.

Client integration (gRPC, cast stream, UDP/group input), in Chinese:
[`INTEGRATION.md`](INTEGRATION.md).

`make -C src/service/cast viewer` builds the desktop viewer;
`./out/service/tebox-cast-view [--host H] --id <instance-id>` opens a window with the live
cast and forwards input: the mouse is the finger, right click / Esc is Back,
Home or F1 is Home, F2 Menu, F5 Power, PgUp/PgDn volume. Typing (including a
Chinese IME) goes to the focused field, Backspace/Delete/Enter/Tab/arrows edit,
Cmd+A/C/X/Z/Y act in the guest and Cmd+V pastes the host clipboard. Clipboards
sync both ways while the window is open.

`ResetInstance` performs a QMP Android reboot by default. Set `wipe_data` to
`true` to stop the instance, remove its per-instance `userdata.img`, and boot it
again as a factory reset. The server binds to loopback by default; set
`TEBOX_GRPC_ADDR` or `--listen` explicitly when exposing it to another process.

Artifacts use only the project FFmpeg under `out/cast-ffmpeg` /
`prebuilts/host/<os-arch>/ffmpeg` (no system Homebrew `ffmpeg` required).

## Cast quick start

```bash
./out/service/tebox-cast --capture-sock "$SOCK" --codec h264 --listen 127.0.0.1:9901 &
TEBOX_CAST=1 TEBOX_CAST_SOCK="$SOCK" TEBOX_ROOT="$PWD" ./run
./out/service/tebox-cast-view --port 9901   # bare tebox-cast port, view only
```
