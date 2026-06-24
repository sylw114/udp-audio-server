# audio_server_udp

Windows 专用低延迟 UDP 音频服务器。服务端通过 TCP 完成会话配置与心跳延迟反馈，通过 UDP 接收音频包，按序号去重/重排后写入无锁环形缓冲区，并使用 WASAPI 渲染到系统默认输出设备。

当前支持两种 UDP 音频负载：

- PCM：16-bit signed little-endian，交错声道。
- Opus：构建时通过 CMake FetchContent 拉取并静态链接 Opus v1.5.2，服务端接收 Opus 帧后解码为 PCM 再渲染。

## 编译

要求：

- Windows
- CMake
- Visual Studio C++ 工具链（`build.bat` 使用 x64 MSVC 生成器）
- 首次构建 Opus 依赖时需要网络访问

在项目根目录运行：

```bat
build.bat
```

构建脚本会配置 `build/cmake-msvc`，构建 Release，并把运行文件安装到 `subbuild/`：

- `subbuild/audio_server_udp.exe`
- `subbuild/THIRD_PARTY_NOTICES.md`

## 用法

```bat
subbuild\audio_server_udp.exe [选项]
```

| 参数 | 值 | 默认值 | 说明 |
|---|---|---|---|
| `--tcp` | `<端口>` | `9000` | TCP 监听端口 |
| `--udp` | `<端口>` | `9000` | UDP 监听端口 |
| `--discard-out-of-order` | - | 关闭 | 丢弃乱序包；默认会缓存并按序释放 |
| `--drop-baseline-duration-ms` | `<ms>` | `0` | 自适应丢帧控制基准时长，`0` 表示禁用 |
| `--protect-ms` | `<ms>` | `50` | 自适应丢帧保护窗口 |

示例：

```bat
subbuild\audio_server_udp.exe --tcp 9000 --udp 9000 --drop-baseline-duration-ms 120 --protect-ms 50
```

## 协议

### 连接流程

1. 客户端先建立 TCP 连接。
2. 客户端发送 `TcpConfigPacket` 配置包。
3. 服务端校验成功后回复 1 字节 ACK：`\0`。
4. 客户端开始通过 UDP 发送音频数据。
5. TCP 任一端断开后，当前音频会话结束；服务端会关闭 UDP socket 以解除 `recvfrom` 阻塞，然后重新创建 UDP socket 等待下一次会话。

### TCP 配置包

配置包长度为 6 字节：

```text
[ version u8 ][ sampleRateIdx u8 ][ channels u8 ][ codec u8 ][ frameMs u8 ][ opusBitrateKbps u8 ]
```

字段说明：

| 字段 | 支持值 | 说明 |
|---|---|---|
| `version` | `0x01` | 当前协议版本 |
| `sampleRateIdx` | `0x00`, `0x01` | `0x00` = 44100 Hz，`0x01` = 48000 Hz |
| `channels` | `0x01`, `0x02` | 单声道或双声道 |
| `codec` | `0x00`, `0x01` | `0x00` = PCM，`0x01` = Opus |
| `frameMs` | PCM 忽略；Opus 支持 `10` / `20` / `40` | Opus 帧长 |
| `opusBitrateKbps` | PCM 忽略 | 当前仅用于配置同步和日志 |

不支持的版本、采样率、声道数、编码格式或 Opus 帧长会导致服务端直接关闭该 TCP 客户端连接。

### UDP 数据包

```text
[ seq u8 ][ payload ... ]
```

- `seq`：`0x00` 到 `0xFF` 循环序号，用于排序、去重和心跳延迟回报。
- PCM 模式：`payload` 为 16-bit signed little-endian PCM，交错声道。
- Opus 模式：`payload` 为单个 Opus 帧；服务端按 TCP 配置中的采样率、声道数和帧长解码。
- 可以对同一帧进行冗余发送；相同或过旧序号会被服务端丢弃。

默认模式会缓存乱序包并等待期望序号；启用 `--discard-out-of-order` 后，收到比期望序号更新的包会跳过中间缺失序号。

### TCP 心跳

客户端可在配置成功后通过 TCP 发送心跳请求。请求长度为 9 字节：

```text
[ seq u8 ][ clientTimestamp u64 BE ]
```

服务端回复：

```text
[ totalLen u16 BE ]
[ seq u8 ][ udpRecvTs u64 BE ] * N
[ serverTs u64 BE ]
```

- `totalLen` 包含自身 2 字节。
- `udpRecvTs` 是服务端收到对应 UDP 序号时记录的 Unix 毫秒时间戳。
- `serverTs` 是服务端发送心跳响应时的 Unix 毫秒时间戳。
- 服务端按上次心跳结束序号到当前 `expectedSeq` 之间的范围回填条目，序号按 `uint8_t` 自动回绕。

## 架构

```text
TCP 配置 / 心跳 ─────> tcpHandler (TCP 线程)
                        ├─ 配置同步：采样率 / 声道 / 编码 / Opus 帧长
                        └─ 延迟反馈响应

UDP 音频流 ─────────> main.cpp (UDP 主线程)
                        ├─ recvfrom 主循环
                        ├─ 序号排序 / 去重 / 乱序处理
                        ├─ PCM 直通或 Opus 解码
                        └─ 写入 RingBuffer (无锁 SPSC)
                                    │
                                    ▼
                        WasapiRenderer (渲染线程)
                        ├─ WASAPI 事件驱动
                        ├─ MMCSS "Pro Audio"
                        ├─ 自适应丢帧延迟控制
                        └─ 欠载时填充静音
```

核心文件：

| 文件 | 说明 |
|---|---|
| `main.cpp` | 入口、CLI 解析、TCP 会话、UDP 接收、排序与生命周期管理 |
| `protocol.h` | 协议结构、编码枚举、采样率映射、序号比较 |
| `opus_decoder.h/cpp` | Opus 解码封装 |
| `wasapi_renderer.h/cpp` | WASAPI 初始化、渲染线程、自适应丢帧算法 |
| `ring_buffer.h` | 无锁 SPSC 环形缓冲区，容量自动对齐至 2 的幂 |
| `packet_queue.h` | 无锁包队列，当前未接入主流程 |
| `CMakeLists.txt` | CMake 构建定义和 Opus FetchContent 配置 |

## 自适应丢帧

当 `--drop-baseline-duration-ms > 0` 时启用。算法根据缓冲区积压量动态提高丢帧概率，以限制端到端延迟：

```text
x = (缓冲时长ms - protectMs) / baselineMs
r = 1 - 1 / exp(x^3 * protectMs / baselineMs)   (x > 0 时)
```

`r` 为每帧丢弃概率。缓冲时长不超过 `protectMs` 时不丢帧。

## 注意事项

- 仅支持 Windows，依赖 Winsock2、WASAPI/MMDevice、MMCSS。
- `wasapi_relink.dll` 为可选运行时依赖；不存在时非致命。
- WASAPI 渲染事件超时 2 秒会触发 `onFatalTimeout`，关闭 TCP 客户端并走正常会话清理流程。
- 序号使用 `uint8_t`，`isNewer()` 采用 14 包前向窗口判断新旧。
- 仓库没有测试和 lint 步骤；修改后至少运行 `build.bat` 验证构建。

## 需要的帮助

1. 当前丢帧更接近“跳帧倍速”，需要更自然的变速/时间伸缩算法。
2. 欠载时静音填充会影响听感，需要更好的预测或 PLC 策略。
