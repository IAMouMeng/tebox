# tebox 投屏与控制对接说明

客户端只需要和 `tebox-api` 打交道，不直接碰 QEMU、adb 或 Android。

| 用途 | 协议 | 默认地址 |
|---|---|---|
| 实例管理（列表、创建、启停）及输入、剪贴板 | gRPC | `:50051` |
| 投屏视频 | TCP | `:9901` |
| 控制（触摸、按键、文字、剪贴板），支持群控 | UDP | `:9903` |

- 默认只监听 `127.0.0.1`，可用 `--listen`、`--cast-listen`、`--input-udp`（或环境变量 `TEBOX_GRPC_ADDR`、`TEBOX_CAST_ADDR`、`TEBOX_INPUT_UDP_ADDR`）改成 `0.0.0.0`。
- R1P 容器默认就监听 `0.0.0.0`。
- 接口没有鉴权，只在可信内网里开放。

gRPC 定义见 [`api/api/v1/tebox.proto`](api/api/v1/tebox.proto)。

## 1. 实例

### 列表：`ListInstances`

每个实例（`Instance`）返回：

| 字段 | 说明 |
|---|---|
| `id` / `name` | 实例 ID，投屏和控制都用它；`name` 是显示名 |
| `state` | `INSTANCE_STATE_STOPPED`、`_STARTING`（QEMU 已启动，Android 开机中）、`_RUNNING`（Android 开机完成）、`_STOPPING` |
| `running` / `pid` | QEMU 进程是否在运行，以及它的进程号 |
| `cpu_cores` / `memory_mb` / `disk_mb` | 创建时设定的核心数、内存、磁盘大小 |
| `disk_used_mb` | 磁盘镜像在宿主上实际占用的空间（按需分配，随数据增长） |
| `adb_port` | adb 端口 |
| `cast_port` / `control_port` | 该实例单独的投屏端口和控制端口（见第 4 节），只在运行时有值 |

### 创建：`CreateInstance`

```json
{ "name": "demo-1", "cpu_cores": 2, "memory_mb": 3072, "disk_mb": 8192 }
```

- 字段不填或填 0 时使用默认值：4 核、4096 MB 内存、8192 MB 磁盘。
- 取值范围：核心数 1 到宿主核心数，内存 1024–65536 MB，磁盘 1024–1048576 MB。超出时返回 `InvalidArgument`。
- 创建完成后状态是停止，需要再调 `StartInstance`。

### 其他接口

`StartInstance`、`StopInstance`、`DeleteInstance`、`ResetInstance`（`wipe_data=true` 表示恢复出厂）。

## 2. 投屏（TCP 9901）

1. 建立 TCP 连接，先发一行：`TEBOX-CAST/1 <实例id>\n`。
2. 之后连接上持续收到视频帧记录，所有整数都是大端：

```
[u32 length][u8 codec][u8 flags][u64 pts_us][payload]
```

| 字段 | 说明 |
|---|---|
| `length` | 从 `codec` 到 `payload` 末尾的字节数（不含 `length` 自己的 4 字节） |
| `codec` | `0` = H.264 |
| `flags` | 第 0 位为 1 表示关键帧 |
| `pts_us` | 时间戳，单位微秒 |
| `payload` | H.264 Annex-B 数据，关键帧里带 SPS/PPS |

- **怎么解码**：把每条记录的 `payload` 依次喂给任意 H.264 解码器即可，例如 FFmpeg、VideoToolbox、MediaCodec、浏览器的 WebCodecs。
- **连上就有画面**：服务端会立刻发一个关键帧；即使画面静止，也会补发最后一帧。
- **默认参数**：720×1600、最高 60fps、8 Mbps，没有 B 帧。分辨率等于手机屏幕 1080×2400 等比缩小。
- **视频源**：Mac 上用 VideoToolbox 编码，R1P 上用 V4L2 硬件编码。
- **同时只能有一个观看端**：第二个连接会排队，直到第一个断开。
- **断线重连**：连接断开（例如 API 重启）后，重新连接并发送握手行就行。

最小示例（Python，把画面交给 ffplay 播放）：

```python
import socket, struct, subprocess
s = socket.create_connection(("10.10.2.142", 9901))
s.sendall(b"TEBOX-CAST/1 tebox-1\n")
player = subprocess.Popen(["ffplay", "-fflags", "nobuffer", "-flags", "low_delay",
                           "-f", "h264", "-"], stdin=subprocess.PIPE)

def read(n):
    buf = b""
    while len(buf) < n:
        chunk = s.recv(n - len(buf))
        if not chunk:
            raise EOFError
        buf += chunk
    return buf

while True:
    (length,) = struct.unpack(">I", read(4))
    body = read(length)
    player.stdin.write(body[10:])  # 跳过 codec(1) + flags(1) + pts(8)
```

## 3. 控制（UDP 9903，支持群控）

每个 UDP 数据报装一个或多个事件，事件是 JSON 对象，多个事件之间用 `\n` 分隔。

### 公共字段

| 字段 | 说明 |
|---|---|
| `id` | 目标实例 |
| `ids` | 多个目标实例，例如 `["a","b"]`；`["*"]` 表示所有运行中的实例。一个包就能群控 |
| `seq` | 可选。每个发送端自己递增的序号，序号不大于上一个的包会被丢弃，避免乱序 |

### 事件

| type | 示例 | 说明 |
|---|---|---|
| `touch` | `{"type":"touch","action":"down","x":0.5,"y":0.25}` | `action` 取 `down`、`move`、`up`；`x`、`y` 是 0–1 的比例，从屏幕左上角算起 |
| `key` | `{"type":"key","key":"home"}` | 导航键，按下并松开。可用 `home`、`back`、`menu`、`power`、`volume_up`、`volume_down`、`mute`，也可直接写 QEMU 按键名（qcode） |
| `text` | `{"type":"text","text":"hello 你好"}` | 输入到当前焦点输入框。英文按键输入，中文等字符通过剪贴板粘贴（会覆盖 guest 剪贴板） |
| `keycode` | `{"type":"keycode","keycode":67,"meta":0,"action":"press"}` | Android 按键码，`action` 取 `down`、`up`、`press`。常用：67 退格、66 回车、61 Tab、19–22 方向键；Ctrl+A 是 keycode 29 加 `meta` 12288（`0x3000`） |
| `clipboard` | `{"type":"clipboard","text":"…","paste":false}` | 设置 guest 剪贴板，`paste` 为 `true` 时同时粘贴 |
| `ping` | `{"type":"ping"}` | 保活，用于维持剪贴板订阅（见下文） |

### 触摸要求（UDP 可能丢包）

- 手指按住期间，至少每秒要发一次事件；没有新坐标时，按当前位置重复发 `move`。超过 1 秒收不到事件，服务端会自动抬起，防止丢了 `up` 导致手指一直按着。参考做法是每 200 ms 发一次。
- `up` 建议连发两次，重复的 `up` 没有副作用。

### 服务端回包（发到发送端的地址）

- 事件出错时回：`{"ok":false,"error":"…","id":"…","seq":N}`。成功不回包。
- guest 剪贴板变化时推送：`{"type":"clipboard","id":"…","text":"…"}`。只推给最近 15 秒内发过包的发送端，所以客户端空闲时要每 5 秒发一次 `ping`。

### 示例

```python
import socket, json, time
u = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
API = ("10.10.2.142", 9903)
seq = 0

def send(**ev):
    global seq
    seq += 1
    ev.update(seq=seq)
    u.sendto((json.dumps(ev, ensure_ascii=False) + "\n").encode(), API)

send(id="tebox-1", type="touch", action="down", x=0.5, y=0.8)    # 单实例点击
send(id="tebox-1", type="touch", action="up", x=0.5, y=0.8)
send(ids=["*"], type="key", key="home")                          # 所有实例回桌面
send(ids=["a", "b"], type="text", text="同时输入")               # 群控输入
```

## 4. 其他接入方式

- **每个实例自己的控制端口 `control_port`**：TCP 和 UDP 用同一个端口号，只监听 127.0.0.1。事件格式同上，但不用带 `id`。TCP 连上后先收到一行 `{"ok":true}`，之后的回包和剪贴板推送都按行返回。
- **9901 上的 TCP 控制**：发 `TEBOX-CTRL/1 <id>\n`，之后的行为和上面的 TCP 控制端口一样。
- **gRPC 控制**：
  - `SendInput`：发单个事件。
  - `StreamInput`：客户端流，适合连续手势。
  - `WatchClipboard`：服务端流，推送 guest 剪贴板变化。
  - 事件类型和 UDP 一一对应：`touch`、`key`、`text`、`keycode`、`clipboard`。

## 5. 参考实现

`cast/tools/cast_view.c` 是一个完整的桌面客户端，大约 900 行 C 代码：SDL2 显示，FFmpeg 解码。用 `make -C src/service/cast viewer` 编译：

```bash
./out/service/tebox-cast-view --host 10.10.2.142 --id tebox-1
```

它包含了上面所有要点：握手、解码、坐标换算、UDP 控制带序号、按住心跳、`up` 连发两次、剪贴板双向同步、断线重连。

坐标换算注意：画面要按比例缩放后居中显示，点击位置先减去黑边，再除以画面显示的宽高，得到 0–1 的比例。macOS Retina 屏上要统一按「点」或统一按「像素」计算。
