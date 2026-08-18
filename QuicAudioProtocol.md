# LiveSuite QUIC 音频协议 v1

该协议只用于 VideoStreamer 与 LiveSuite 的低延迟音频监听，不属于
`stream-server`。接收器默认使用 QUIC，可用 `--transport udp` 切回旧的
TCP + UDP 协议：

```text
audio_server_udp.exe --transport quic --udp 9000
audio_server_udp.exe --transport udp --tcp 9000 --udp 9000
```

## 连接与配置

- QUIC ALPN：`livesuite-audio-v1`
- 证书：接收器启动时生成临时自签名证书；内网客户端不校验证书
- 媒体与控制：共用首个可靠、有序的双向 QUIC Stream
- 分帧：所有消息使用 4 字节大端长度前缀

客户端首条控制消息为 22 字节：

| 字段 | 长度 | 说明 |
| --- | ---: | --- |
| 类型 | 1 | `0x01` |
| 版本 | 1 | `0x01` |
| 会话 ID | 8 | 大端无符号整数 |
| 采样率 | 4 | 44100 或 48000 |
| 声道 | 1 | 1 或 2 |
| 编码 | 1 | `0x00` PCM，`0x01` Opus |
| 帧长 | 1 | Opus 为 10 / 20 / 40 ms |
| Opus 码率 | 4 | bit/s |
| 保留 | 1 | v1 固定为 0 |

服务端以 `0x81 + 会话 ID + 最大 Stream 消息长度(u16)` 确认。

## 音频 Stream 帧

```text
type=0x10 | sessionId(u64) | seq(u8) | originalSendEpochMs(i64)
          | length(u16) | payload
```

每个音频 access unit 是一条长度分帧消息。QUIC Stream 自身负责可靠传输、
重排和去重，因此应用层不重复发包，也不维护媒体重排窗口。序号仅用于诊断
协议异常，可靠重传造成的等待会直接体现在延迟范围统计中。

PCM 会按协商的 Stream 消息上限拆包；Opus 每个 access unit 独立成帧。低延迟
与音质兼顾的默认值是 48 kHz、双声道、Opus 96 kbit/s、关闭冗余。客户端会
把请求帧长解析为设备编码器实际支持的值，并在握手中发送实际值。

## 延迟范围

客户端每 500 ms 在可靠控制流上进行一次四时间戳同步：

1. 客户端发送 `t0`；服务端记录 `t1` 并以 `t0/t1/t2` 回复。
2. 客户端在 `t3` 收到回复，计算服务端相对客户端时钟偏移区间。
3. 服务端结合每帧的原始发送时间和接收时间，得到单向网络延迟的上下界。
4. 服务端每 250 ms 回传该窗口的延迟最小/最大值、收包、丢包、恢复和抖动统计。

该范围包含网络排队和时钟同步误差，不包含 Android 音频采集前与 WASAPI
渲染后的设备路径延迟。
