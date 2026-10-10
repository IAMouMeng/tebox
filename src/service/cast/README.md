# Host cast (`tebox-cast`)

Guest-unaware screen casting for the QEMU VirGL path: scanout is captured on
the host, encoded with **bundled** FFmpeg (`thirdparty/ffmpeg`), and streamed
as length-prefixed bitstreams (not WebRTC).

This is one feature under [`src/service/`](../) (future: emulator create/delete
client, etc.).

## Build

```bash
./scripts/build-ffmpeg-cast.sh   # thirdparty/ffmpeg → out/cast-ffmpeg + prebuilts
make -C src/service/cast
```

Binaries: `out/service/tebox-cast`, `out/service/tebox-cast-view`  
FFmpeg: `out/cast-ffmpeg` and `prebuilts/host/<os-arch>/ffmpeg`

## Run

```bash
SOCK="$PWD/out/test-$VARIANT/cast.sock"
./out/service/tebox-cast --capture-sock "$SOCK" --listen 127.0.0.1:9901 &
TEBOX_CAST=1 TEBOX_CAST_SOCK="$SOCK" TEBOX_CAST_FPS=60 ./run
TEBOX_ROOT="$PWD" ./out/service/tebox-cast-view --port 9901
```

Env: `TEBOX_CAST`, `TEBOX_CAST_SOCK`, `TEBOX_CAST_FPS`, `TEBOX_CAST_MAX_W/H`,
`TEBOX_CAST_FLIP=0` to disable CPU vertical flip.

## Wire formats

See `include/cast/protocol.h`. Encoded TCP:
`[u32 be length][u8 codec][u8 flags][u64 be pts_us][payload]`.

Control JSON on a second TCP port (default 9902).
