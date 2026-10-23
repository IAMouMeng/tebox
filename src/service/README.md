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
The API exposes three TCP endpoints. A client selects the Android instance
immediately after connecting by sending one handshake line; the remaining bytes
use the encoded cast record format from
[`cast/include/cast/protocol.h`](cast/include/cast/protocol.h)
(`[u32 len][u8 codec][u8 flags][u64 pts][payload]`) or, for input, JSON lines:

| Endpoint (default, flag / env) | Handshake | Server → client | Client → server |
|---|---|---|---|
| video `127.0.0.1:9901` (`--cast-listen` / `TEBOX_CAST_ADDR`) | `TEBOX-CAST/1 <id>` | H.264 records only | nothing (ignored) |
| audio `127.0.0.1:9904` (`--audio-listen` / `TEBOX_AUDIO_ADDR`) | `TEBOX-AUDIO/1 <id>` | AAC records only | microphone PCM records |
| control `127.0.0.1:9905` (`--control-listen` / `TEBOX_CONTROL_ADDR`) | `TEBOX-CTRL/1 <id>` | input replies, clipboard | input JSON lines |
| control (same port) | `TEBOX-CAMERA/1 <id>` | nothing | camera JPEG records |
| control, UDP (same number) | none; each datagram names `"id"` | errors, clipboard | input JSON datagrams |

The video endpoint never accepts input or uplink data.
`Instance.cast_port` reports the per-instance `tebox-cast` port while it runs.
Up to 16 viewers can watch one instance at once. Each viewer receives a
keyframe as soon as it connects. A viewer that falls about 4 MB behind is
dropped, so it can never stall the others.

Record codecs (values in `protocol.h`):

| Direction | codec | Payload |
|---|---|---|
| server → viewer | 0 | H.264 video (Annex-B) |
| server → viewer | 16 | guest audio, AAC-LC 48 kHz stereo, one ADTS frame per record |
| viewer → server | 32 | camera frame, one JPEG of any size (scaled and centre-cropped to 1280×720) |
| viewer → server | 33 | microphone, PCM s16le 48 kHz mono, ~10–20 ms per record |

Clients send upstream records in the same framing after the handshake line
(microphone on the audio endpoint, camera on the control endpoint); records of
any other codec are dropped. Any client can feed the camera or microphone;
the latest frame wins. Without camera frames the guest camera shows a moving
test pattern; without microphone data it records silence.

Inside the guest these travel over virtio-serial ports (`tebox.audio_out`,
`tebox.audio_in`, `tebox.camera`; sockets `audio-out.sock`, `audio-in.sock`,
`camera.sock` next to `cast.sock`) to the audio HAL and the camera HAL.

Guest input (touch and keys) has three entry points, all feeding one
dedicated QMP monitor per instance (`qmp-input.sock`, from `QMP_INPUT_SOCKET`),
so input never holds the lifecycle QMP socket:

- `Instance.control_port`: plain TCP on 127.0.0.1. The server greets with
  `{"ok":true}`, then reads one JSON object per line and answers only on
  errors (`{"ok":false,"error":...}`).
- `TEBOX-CTRL/1 <instance-id>\n` on the control endpoint (9905), then the same lines.
- UDP datagrams to the control port number (9905), each event naming its
  instance with `"id"` (plus an increasing `"seq"`).

Navigation keys, Android keycodes, text and clipboard use this TCP/UDP
path (`Instance.control_port`, `TEBOX-CTRL/1`, UDP on the same port).
gRPC `GetPreview` returns a still of the guest display (`size` / `format` /
`quality`). `GetInstance` returns the same `Instance` as `ListInstances`,
including `adb_port`, `disk_mb`, `disk_used_mb` and `disk_free_mb`.
`SendKey` / `PowerDown` are QEMU QMP qcodes.

One JSON object per line:

| Event | Example | Path |
|------|---------|------|
| touch | `{"type":"touch","action":"down","x":0.5,"y":0.25}` (`down`/`move`/`up`, 0..1 from top-left) | QEMU virtio touchscreen |
| key | `{"type":"key","key":"home"}` (`home`, `back`, `menu`, `power`, `volume_up`, `volume_down`, `mute`, or a QEMU qcode) | QEMU virtio keyboard |
| text | `{"type":"text","text":"hi 你好"}` (ASCII is typed; other text is pasted) | guest agent |
| keycode | `{"type":"keycode","keycode":67,"meta":0,"action":"press"}` (Android `KEYCODE_*`/`META_*`; `down`/`up`/`press`) | guest agent |
| clipboard | `{"type":"clipboard","text":"...","paste":false}` | guest agent |

Guest clipboard changes come back on the same connection as
`{"type":"clipboard","text":"..."}`; text set from the
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

`CreateInstance` takes optional `system_image`, `vendor_image` and
`initramfs_image`: absolute paths on the API host (empty = the variant's images
under `src/aosp/<variant>/images/`). They are stored with the instance and passed
to `./run` as `SYSTEM_IMG` / `VENDOR_IMG` / `INITRD_IMG`; system and vendor stay
read-only, so instances can share them.

Artifacts use only the project FFmpeg under `out/cast-ffmpeg` /
`prebuilts/host/<os-arch>/ffmpeg` (no system Homebrew `ffmpeg` required).

## Cast quick start

```bash
./out/service/tebox-cast --capture-sock "$SOCK" --codec h264 --listen 127.0.0.1:9901 &
TEBOX_CAST=1 TEBOX_CAST_SOCK="$SOCK" TEBOX_ROOT="$PWD" ./run
./out/service/tebox-cast-view --port 9901   # bare tebox-cast port: video + audio on one socket
```
