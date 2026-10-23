# tebox-api

tebox-api 是 gRPC 服务，用来管理 Android 实例：创建、删除、启动、停止、重置，并负责输入和投屏。
接口定义见 [`api/v1/tebox.proto`](api/v1/tebox.proto)（`package tebox.v1`，
`service AndroidService`）。服务开启了 gRPC reflection，所以 `grpcurl` 不需要 proto 文件也能直接调用。

## 地址

| 端口 | 本机默认 | R1P `tebox-1` | 用途 |
|------|----------|---------------|------|
| gRPC | `127.0.0.1:50051` | `10.10.2.142:50051` | 本文档的接口 |
| 视频 | `127.0.0.1:9901` | `10.10.2.142:9901` | 只出 H.264，不接收任何数据 |
| 音频 | `127.0.0.1:9904` | `10.10.2.142:9904` | 出 AAC，收麦克风 PCM |
| 控制 | `127.0.0.1:9905` | `10.10.2.142:9905` | TCP：输入 JSON 行、摄像头 JPEG 上传；UDP（同一端口号）：输入 JSON 数据报 |

地址可以用 `--listen` / `--cast-listen` / `--audio-listen` / `--control-listen` 修改，
也可以用环境变量 `TEBOX_GRPC_ADDR` / `TEBOX_CAST_ADDR` / `TEBOX_AUDIO_ADDR` / `TEBOX_CONTROL_ADDR` 修改。
控制地址同时用于 TCP 和 UDP。
在 R1P 上再部署第二个容器时，用 `R1P_GRPC_PORT` / `R1P_CAST_PORT` / `R1P_AUDIO_PORT` /
`R1P_CONTROL_PORT` 给它分配另一组端口（容器共用宿主网络）。

实例元数据（`instance.json`）放在 `TEBOX_DATA_DIR/instances/<id>/` 下。
`TEBOX_DATA_DIR` 默认是 `~/.tebox`，R1P 上是 `/data` = `/mmc/tebox/tebox-1`。
实例的运行目录（userdata、socket）默认也在这里，可以在创建时用 `instance_dir` 另外指定。

下文示例都用这个函数：

```bash
API=10.10.2.142:50051
g() { grpcurl -plaintext -d "${2:-{\}}" "$API" tebox.v1.AndroidService/$1; }
```

## 接口一览

| RPC | 请求 | 返回 | 说明 |
|-----|------|------|------|
| `ListInstances` | `{}` | `instances[]` | 列出所有实例和状态 |
| `GetInstance` | `{id}` | `Instance` | 单个实例：adb 端口、磁盘占用、运行状态 |
| `CreateInstance` | `CreateInstanceRequest` | `Instance` | 只创建实例，不启动 |
| `DeleteInstance` | `{id}` | `{}` | 停止实例，删除元数据和运行目录（包括 userdata） |
| `StartInstance` | `{id}` | `{}` | 启动 QEMU；Android 开机完成后状态变为 RUNNING |
| `StopInstance` | `{id}` | `{}` | 先发 ACPI 关机，再结束 QEMU |
| `ResetInstance` | `{id, wipe_data}` | `{}` | 重启；`wipe_data=true` 时恢复出厂（删除 userdata 后重新启动） |
| `ConfigureInstance` | `{id, profile}` | `{}` | 设置位置、运营商、号码，以及是否 root |
| `SendKey` / `PowerDown` | `{id, key}` / `{id}` | `{}` | QMP 按键 / 关机（QEMU qcode） |
| `GetPreview` | `{id, size, format, quality}` | `{image, format, mime_type, width, height}` | 当前画面截图；缩放与编码见下文 |

## 创建实例

```protobuf
message CreateInstanceRequest {
  string name = 1;
  int32 cpu_cores = 2;         // 1..宿主核数，默认 4
  int32 memory_mb = 3;         // 1024..65536，默认 4096
  int32 disk_mb = 4;           // userdata 大小 1024..1048576，默认 8192
  string system_image = 5;     // system.img 的绝对路径
  string vendor_image = 6;     // vendor.img 的绝对路径
  string initramfs_image = 7;  // initramfs.img 的绝对路径
  string instance_dir = 8;     // 实例运行目录的绝对路径
}
```

### 运行目录 `instance_dir`

运行目录里放 `userdata.img`，以及 QMP、投屏、音频、摄像头用的 Unix socket。
不填时用 `TEBOX_DATA_DIR/instances/<id>/`。

- 必须是 API 所在机器上的绝对路径。
- 目录要么不存在（会自动创建），要么是空目录。
  创建后目录里会写一个 `.tebox-instance` 标记，所以同一个目录不能给两个实例用。
- 路径长度不能超过 88 字节，否则 Unix socket 路径（`<dir>/qmp-input.sock`）会超过系统上限。
- `DeleteInstance` 会删除整个运行目录，删除前会核对目录里的标记。
- 在 R1P 上，运行目录要放在 `/mmc/tebox/instances/` 下：这个目录以相同路径读写挂载进了容器。
  例如 `"instance_dir": "/mmc/tebox/instances/phone-1"`。
- 返回的 `Instance.dir` 就是运行目录。

- 三个镜像路径都可以不填。不填的那项会用变体自带的镜像：
  `src/aosp/<variant>/images/{system,vendor,initramfs}.img`（R1P 上就是 `/mmc/tebox/images/` 里的镜像）。
- 路径是 **API 所在机器上** 的路径，而且必须是绝对路径、非空的普通文件。
  不满足时，创建会直接返回 `InvalidArgument`。每次启动前还会再检查一遍，文件不在了会启动失败。
- system 和 vendor 以只读方式挂载，initramfs 也只读取。所以多个实例可以共用同一份镜像，
  API 也不会修改这些镜像。
- 在 R1P 上，镜像要放在 `/mmc/tebox/user-images/` 下（容器里同一路径只读挂载）：

  ```bash
  scp system.img vendor.img initramfs.img user@10.10.2.142:/tmp/
  ssh user@10.10.2.142 'sudo mv /tmp/{system,vendor,initramfs}.img /mmc/tebox/user-images/'
  ```

- 实例的内核和 cmdline 仍然由变体决定（`src/aosp/<variant>/KERNEL`、`qemu/cmdline/boot`）。
  vendor 和 initramfs 应该和这个内核配套，一般用 `scripts/build-vendor-img.sh` / `build-initramfs.sh` 生成。

```bash
g CreateInstance '{
  "name": "phone-1", "cpu_cores": 4, "memory_mb": 4096, "disk_mb": 8192,
  "system_image":    "/mmc/tebox/user-images/system.img",
  "vendor_image":    "/mmc/tebox/user-images/vendor.img",
  "initramfs_image": "/mmc/tebox/user-images/initramfs.img",
  "instance_dir":    "/mmc/tebox/instances/phone-1"
}'
g StartInstance '{"id":"android-1791942209-289"}'
```

返回的 `Instance`（`ListInstances` 与 `GetInstance` 相同）：

| 字段 | 含义 |
|------|------|
| `id`, `name`, `variant`, `dir` | 标识，以及运行目录 |
| `state` | `STOPPED` / `STARTING`（QEMU 已起，Android 开机中）/ `RUNNING`（`sys.boot_completed=1`）/ `STOPPING` |
| `adb_port` | `adb connect <host>:<adb_port>`（R1P 上对局域网开放） |
| `cast_port`, `control_port` | 运行时才有：实例自己的 tebox-cast 端口，以及输入端口（只在本机 127.0.0.1 上监听） |
| `cpu_cores`, `memory_mb` | CPU 核数、内存 |
| `disk_mb` | userdata 分配大小（MB） |
| `disk_used_mb` | userdata.img 在宿主上实际占用的空间（稀疏文件按块计） |
| `disk_free_mb` | `max(0, disk_mb - disk_used_mb)` |
| `system_image`, `vendor_image`, `initramfs_image` | 实例实际启动用的镜像（未指定时是变体默认镜像的路径） |
| `profile` | `ConfigureInstance` 设置的设备信息 |

```bash
g ListInstances
g GetInstance '{"id":"android-1791942209-289"}'
```

## 预览画面

`GetPreview` 抓当前客户机画面。实例必须已在运行（QEMU 进程在）。优先 `adb exec-out screencap -p`，失败则走 QMP `screendump`。

```protobuf
message GetPreviewRequest {
  string id = 1;
  int32 size = 2;      // 缩放后最长边像素；0 为原尺寸，上限 4096
  string format = 3;   // jpeg（默认）或 png
  int32 quality = 4;   // 1..100，仅 jpeg，默认 80
}
```

`size` 是最长边：等比缩小或放大，使 `max(宽, 高) = size`。`image` 是编码后的字节（grpcurl JSON 里是 base64）。返回的 `width` / `height` 是缩放后的实际像素。

```bash
g GetPreview '{"id":"tebox-1"}'
g GetPreview '{"id":"tebox-1","size":540,"format":"jpeg","quality":70}'
g GetPreview '{"id":"tebox-1","format":"png"}'
grpcurl -plaintext -d '{"id":"tebox-1","format":"jpeg","quality":80,"size":540}' \
  "$API" tebox.v1.AndroidService/GetPreview | jq -r .image | base64 -d > preview.jpg
```

导航键、Android keycode、文本、剪贴板走 TCP/UDP 控制口（见下文）。

## 投屏、音频与输入

每个 TCP 连接建立后，先发一行握手 `<协议> <实例id>\n`。
之后传的是二进制记录 `[u32 大端长度][u8 codec][u8 flags][u64 大端 pts_us][payload]`（长度从 codec 算起），
或者是 JSON 行（控制）。

| 端口 | 握手 | 服务端 → 客户端 | 客户端 → 服务端 |
|------|------|-----------------|-----------------|
| 9901 视频 | `TEBOX-CAST/1 <id>` | codec 0：H.264 Annex-B（连上即发关键帧） | 不接收（发了也会被丢弃） |
| 9904 音频 | `TEBOX-AUDIO/1 <id>` | codec 16：AAC-LC 48 kHz 立体声，每条一个 ADTS 帧 | codec 33：麦克风 PCM s16le 48 kHz 单声道，每条 10–20 ms |
| 9905 控制 | `TEBOX-CTRL/1 <id>` | 先回 `{"ok":true}`；之后只在出错时回复，客户机剪贴板变化也从这里推送 | 每行一个输入 JSON |
| 9905 控制 | `TEBOX-CAMERA/1 <id>` | 无 | codec 32：摄像头帧，每条一张 JPEG（任意尺寸） |
| 9905 控制（UDP） | 无握手，每个数据报都带 `"id"` | 出错回复和剪贴板推送，发回发送方地址 | 输入 JSON（一个数据报可以放多行） |

- 每个端口只转发表里列出的 codec，其他记录直接丢弃。视频端口不接受控制和上行数据。
- 客户机没有声音时，音频端口不会发数据。
- 同一实例最多可以有 16 个连接同时观看或收听。某个连接落后约 4 MB 时会被断开，不会拖慢其他连接。
- 输入 JSON 示例：
  - `{"type":"touch","action":"down","x":0.5,"y":0.25}`（x、y 取 0..1，从左上角算起）
  - `{"type":"key","key":"home"}`
  - `{"type":"text","text":"你好"}`
  - `{"type":"keycode","keycode":67}`
  - `{"type":"clipboard","text":"...","paste":false}`

  完整格式见 [`../README.md`](../README.md)。
- UDP 输入发到 9905（和 TCP 控制同一个端口号）。
  - 每个数据报都要带 `"id"`。
  - `"seq"` 要一直递增：服务端按“发送地址 + 实例”记录，`seq` 不大于上一个的包会被丢弃。
  - 单个数据报不超过 64 KB。
  - 想收到剪贴板推送，至少每 15 秒发一个包。空闲时发 `{"id":"...","type":"ping"}` 就行。

  例如：`{"id":"tebox-1","seq":1,"type":"touch","action":"down","x":0.5,"y":0.5}`

导航键 / Android keycode / 文本 / 剪贴板只走这条 TCP/UDP 控制口（`Instance.control_port`，或 `TEBOX-CTRL/1 <id>`）。gRPC 没有 `SendInput`。

```bash
# 实例自己的输入口（ListInstances / GetInstance 里的 control_port）
printf '%s\n' '{"type":"key","key":"home"}' | nc 127.0.0.1 "$CTRL"
printf '%s\n' '{"type":"text","text":"hello"}' | nc 127.0.0.1 "$CTRL"
# 或共享控制口 9905
printf 'TEBOX-CTRL/1 tebox-1\n{"type":"key","key":"home"}\n' | nc 10.10.2.142 9905
# 桌面查看器：视频 9901，音频 9904，摄像头和输入都走 9905（TCP / UDP）
./out/service/tebox-cast-view --host 10.10.2.142 --id tebox-1 \
    [--audio-port 9904] [--control-port 9905] [--mic] [--camera avfoundation:0]
```

## 错误码

| gRPC code | 情况 |
|-----------|------|
| `InvalidArgument` | 参数超出范围；镜像路径或 `instance_dir` 不合法；实例 id 不合法；`GetPreview` 的 format 不是 jpeg/png |
| `NotFound` | 实例不存在 |
| `FailedPrecondition` | 实例没在运行（`GetPreview`） |
| `Internal` | 其他错误（QMP、复制失败等），原因在 message 里 |

## 构建与部署

```bash
make -C src/service/api                 # → out/service/tebox-api（本机）
scripts/r1p-deploy.sh bin               # 交叉编译并上传到 R1P，然后重建 tebox-1 容器（实例会重启）
```

修改 `api/v1/tebox.proto` 后要重新生成代码（在 `api/v1/` 目录下执行）：

```bash
protoc --go_out=. --go_opt=paths=source_relative \
       --go-grpc_out=. --go-grpc_opt=paths=source_relative tebox.proto
```
