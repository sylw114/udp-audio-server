# audio_server_udp

Windows 专用低延迟 UDP 音频直通服务器。通过 UDP 接收 16-bit PCM 音频数据，缓冲后经 WASAPI 渲染至系统默认输出设备；TCP 连接负责会话握手、配置同步与心跳延迟测量。

## 编译

需要安装 Visual Studio（含 C++ 工具集）。

```bat
build.bat
```

输出：`subbuild/audio_server_udp.exe`

编译器选项：`/EHsc /O2 /utf-8 /std:c++17 /W3`  
链接库：`ws2_32.lib ole32.lib avrt.lib uuid.lib`

## 用法

```
audio_server_udp.exe [选项]
```

| 参数 | 值 | 默认值 | 说明 |
|---|---|---|---|
| `--tcp` | `<端口>` | `9000` | TCP 监听端口 |
| `--udp` | `<端口>` | `9000` | UDP 监听端口 |
| `--discard-out-of-order` | — | 关闭 | 丢弃乱序包（默认：缓存后重排） |
| `--drop-baseline-duration-ms` | `<ms>` | `0` | 自适应丢帧控制基准时长，`0` 表示禁用 |
| `--protect-ms` | `<ms>` | `50` | 自适应丢帧保护窗口（ms） |

## 协议

### 连接流程

1. 客户端先建立 **TCP** 连接，发送 2 字节配置包：
   - `sampleRateIdx`（uint8）：`0x00` = 44100 Hz，`0x01` = 48000 Hz
   - `channels`（uint8）：声道数
2. 服务端回复单字节 ACK（`\0`）。
3. 客户端开始通过 **UDP** 发送音频数据。
4. TCP 任一端断开，整个音频会话结束。

### UDP 数据包格式

```
[ seq (1 byte) ][ PCM 数据 (N bytes) ]
```

- `seq`：0x00–0xFF 循环序号，用于排序与去重。
- PCM：16-bit 有符号整数，小端序，交错声道。
- 每帧建议三次冗余发送（相同序号），服务端自动去重。

### TCP 心跳格式

客户端可随时发送心跳请求（9 字节）：

```
[ seq (1 byte) ][ clientTimestamp (8 bytes BE) ]
```

服务端回复：

```
[ totalLen (2 bytes BE) ]
[ seq (1 byte) + udpRecvTs (8 bytes BE) ] × N
[ serverTs (8 bytes BE) ]
```

时间戳为毫秒单位。起点任意。

## 架构

```
UDP 音频流 ──────────────────────────────────────────────────────┐
                                                                  ▼
                        main.cpp (UDP 接收线程)
                        ├─ recvfrom 主循环
                        ├─ 序号排序 / 乱序丢弃
                        └─ 写入 RingBuffer (无锁 SPSC)
                                    │
                                    ▼
                        WasapiRenderer (渲染线程)
                        ├─ WASAPI 事件驱动，MMCSS "Pro Audio"
                        ├─ 自适应丢帧延迟控制
                        └─ 欠载时填充静音

TCP 握手 / 心跳 ──────> tcpHandler (TCP 线程)
                        ├─ 配置同步 (采样率 / 声道)
                        └─ 延迟反馈响应
```

**线程数：** 3（UDP 主线程、TCP 处理线程、WASAPI 渲染线程）

### 核心模块

| 文件 | 说明 |
|---|---|
| `main.cpp` | 入口，CLI 解析，UDP 接收与排序，生命周期管理 |
| `wasapi_renderer.h/cpp` | WASAPI 初始化、渲染线程、自适应丢帧算法 |
| `ring_buffer.h` | 无锁 SPSC 环形缓冲区，容量自动对齐至 2 的幂 |
| `protocol.h` | 网络包结构体、序号比较（14 包前向窗口）、采样率映射 |
| `packet_queue.h` | 无锁包队列（当前未使用） |

### 自适应丢帧算法

当 `--drop-baseline-duration-ms > 0` 时启用。根据缓冲区积压量动态丢帧以控制延迟：

```
x = (缓冲时长ms - protectMs) / baselineMs
r = 1 - 1 / exp(x³ × protectMs / baselineMs)   (x > 0 时)
```

`r` 为每帧的丢弃概率。缓冲时长不超过 `protectMs` 时不丢帧。

### RingBuffer

- SPSC 无锁设计，生产者（UDP 线程）与消费者（WASAPI 渲染线程）分别持有独立的原子索引。
- 两段式 memcpy 处理环绕。

## 注意事项

- 仅支持 Windows（依赖 Winsock2、WASAPI/MMDevice、MMCSS）。
- `wasapi_relink.dll` 为可选运行时依赖，不存在时非致命。
- WASAPI 渲染事件超时 2 秒将触发 `onFatalTimeout`，关闭 TCP 连接并级联终止整个会话。
- 序号使用 `uint8_t`（0–255 循环），`isNewer()` 采用 14 包前向窗口判断新旧。
- TCP 客户端断开时 UDP socket 会被关闭并重建（用于解除 `recvfrom` 阻塞）。

# 需要的帮助

1. 音频帧跳过实际上是倍速，需要更好的倍速算法
2. 静音填充不太合理，影响听感，需要预测算法
