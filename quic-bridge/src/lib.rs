use anyhow::{anyhow, Context, Result};
use quinn::crypto::rustls::QuicServerConfig;
use rustls::pki_types::{CertificateDer, PrivateKeyDer, PrivatePkcs8KeyDer};
use std::ffi::c_char;
use std::net::{Ipv4Addr, SocketAddr};
use std::ptr;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{self, Receiver, RecvTimeoutError, Sender};
use std::sync::{Arc, Mutex, OnceLock};
use std::thread::{self, JoinHandle};
use std::time::{Duration, SystemTime, UNIX_EPOCH};
use tokio::io::{AsyncReadExt, AsyncWriteExt};

const ALPN: &[u8] = b"livesuite-audio-v1";
const VERSION: u8 = 1;
const MAX_CONTROL_FRAME: usize = 64 * 1024;
const MAX_AUDIO_PAYLOAD: usize = 64 * 1024;

const CONTROL_HELLO: u8 = 0x01;
const CONTROL_SYNC_REQUEST: u8 = 0x02;
const CONTROL_SYNC_RESULT: u8 = 0x03;
const CONTROL_STOP: u8 = 0x04;
const CONTROL_HELLO_ACK: u8 = 0x81;
const CONTROL_SYNC_RESPONSE: u8 = 0x82;
const CONTROL_STATS: u8 = 0x83;
const CONTROL_AUDIO: u8 = 0x10;

const EVENT_CONFIG: u32 = 1;
const EVENT_PACKET: u32 = 2;
const EVENT_DISCONNECTED: u32 = 3;
const EVENT_ERROR: u32 = 4;

#[repr(C)]
pub struct LsAudioQuicEvent {
    pub kind: u32,
    pub sample_rate: u32,
    pub bitrate: u32,
    pub sent_at_epoch_ms: i64,
    pub payload_length: u32,
    pub sequence: u8,
    pub channels: u8,
    pub codec: u8,
    pub frame_ms: u8,
}

#[derive(Clone, Copy)]
struct AudioConfig {
    session_id: u64,
    sample_rate: u32,
    bitrate: u32,
    channels: u8,
    codec: u8,
    frame_ms: u8,
}

struct AudioFrame {
    sequence: u8,
    sent_at_epoch_ms: i64,
    payload: Vec<u8>,
}

enum BridgeEvent {
    Config(AudioConfig),
    Packet(AudioFrame),
    Disconnected,
    Error(String),
}

pub struct ServerHandle {
    receiver: Mutex<Receiver<BridgeEvent>>,
    running: Arc<AtomicBool>,
    disconnect_requested: Arc<AtomicBool>,
    thread: Option<JoinHandle<()>>,
    last_error: Arc<Mutex<String>>,
}

struct SessionStats {
    clock_bounds: Option<(i64, i64)>,
    expected_sequence: Option<u8>,
    missing: [bool; 256],
    last_seen_at: [i64; 256],
    last_sequence: u8,
    received: u64,
    lost: u64,
    recovered: u64,
    duplicates: u64,
    latency_minimum: i32,
    latency_maximum: i32,
    previous_transit: Option<i64>,
    jitter: f64,
}

impl SessionStats {
    fn new() -> Self {
        Self {
            clock_bounds: None,
            expected_sequence: None,
            missing: [false; 256],
            last_seen_at: [0; 256],
            last_sequence: 0,
            received: 0,
            lost: 0,
            recovered: 0,
            duplicates: 0,
            latency_minimum: -1,
            latency_maximum: -1,
            previous_transit: None,
            jitter: 0.0,
        }
    }

    fn accept(&mut self, frame: &AudioFrame, received_at: i64) -> bool {
        let index = frame.sequence as usize;
        if received_at.saturating_sub(self.last_seen_at[index]) < 500 {
            self.duplicates += 1;
            return false;
        }
        self.last_seen_at[index] = received_at;

        match self.expected_sequence {
            None => self.expected_sequence = Some(frame.sequence.wrapping_add(1)),
            Some(expected) if frame.sequence == expected => {
                self.expected_sequence = Some(expected.wrapping_add(1));
            }
            Some(expected) if sequence_is_newer(expected.wrapping_sub(1), frame.sequence) => {
                let gap = frame.sequence.wrapping_sub(expected) as usize;
                for offset in 0..gap.min(64) {
                    self.missing[expected.wrapping_add(offset as u8) as usize] = true;
                }
                self.lost += gap as u64;
                self.expected_sequence = Some(frame.sequence.wrapping_add(1));
            }
            Some(_) if self.missing[index] => {
                self.missing[index] = false;
                self.recovered += 1;
            }
            Some(_) => return false,
        }

        self.received += 1;
        self.last_sequence = frame.sequence;
        if let Some((offset_minimum, offset_maximum)) = self.clock_bounds {
            let minimum = received_at
                .saturating_sub(frame.sent_at_epoch_ms.saturating_add(offset_maximum))
                .clamp(0, 60_000) as i32;
            let maximum = received_at
                .saturating_sub(frame.sent_at_epoch_ms.saturating_add(offset_minimum))
                .clamp(0, 60_000) as i32;
            self.latency_minimum = if self.latency_minimum < 0 {
                minimum
            } else {
                self.latency_minimum.min(minimum)
            };
            self.latency_maximum = self.latency_maximum.max(maximum);

            let midpoint = offset_minimum.saturating_add(offset_maximum) / 2;
            let transit =
                received_at.saturating_sub(frame.sent_at_epoch_ms.saturating_add(midpoint));
            if let Some(previous) = self.previous_transit {
                let delta = transit.saturating_sub(previous).unsigned_abs() as f64;
                self.jitter += (delta - self.jitter) / 16.0;
            }
            self.previous_transit = Some(transit);
        }
        true
    }

    fn stats_payload(&mut self) -> Vec<u8> {
        let mut data = Vec::with_capacity(26);
        data.push(CONTROL_STATS);
        data.push(self.last_sequence);
        push_i32(&mut data, self.latency_minimum);
        push_i32(&mut data, self.latency_maximum);
        push_u32(&mut data, self.received.min(u32::MAX as u64) as u32);
        push_u32(&mut data, self.lost.min(u32::MAX as u64) as u32);
        push_u32(&mut data, self.recovered.min(u32::MAX as u64) as u32);
        push_i32(
            &mut data,
            self.jitter.round().clamp(0.0, i32::MAX as f64) as i32,
        );
        self.latency_minimum = -1;
        self.latency_maximum = -1;
        data
    }
}

static GLOBAL_ERROR: OnceLock<Mutex<String>> = OnceLock::new();

fn global_error() -> &'static Mutex<String> {
    GLOBAL_ERROR.get_or_init(|| Mutex::new(String::new()))
}

#[no_mangle]
pub extern "C" fn ls_audio_quic_server_start(port: u16) -> *mut ServerHandle {
    if port == 0 {
        set_error(global_error(), "QUIC 端口不能为 0");
        return ptr::null_mut();
    }

    let (event_sender, event_receiver) = mpsc::channel();
    let (ready_sender, ready_receiver) = mpsc::sync_channel(1);
    let running = Arc::new(AtomicBool::new(true));
    let disconnect_requested = Arc::new(AtomicBool::new(false));
    let last_error = Arc::new(Mutex::new(String::new()));
    let thread_running = running.clone();
    let thread_disconnect = disconnect_requested.clone();
    let thread_error = last_error.clone();
    let thread = match thread::Builder::new()
        .name("LiveSuite-audio-quic".to_string())
        .spawn(move || {
            run_server_thread(
                port,
                event_sender,
                ready_sender,
                thread_running,
                thread_disconnect,
                thread_error,
            );
        }) {
        Ok(thread) => thread,
        Err(error) => {
            set_error(global_error(), &format!("无法创建 QUIC 线程：{error}"));
            return ptr::null_mut();
        }
    };

    match ready_receiver.recv_timeout(Duration::from_secs(10)) {
        Ok(Ok(())) => Box::into_raw(Box::new(ServerHandle {
            receiver: Mutex::new(event_receiver),
            running,
            disconnect_requested,
            thread: Some(thread),
            last_error,
        })),
        Ok(Err(message)) => {
            set_error(global_error(), &message);
            running.store(false, Ordering::Release);
            let _ = thread.join();
            ptr::null_mut()
        }
        Err(error) => {
            set_error(global_error(), &format!("等待 QUIC 启动超时：{error}"));
            running.store(false, Ordering::Release);
            let _ = thread.join();
            ptr::null_mut()
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn ls_audio_quic_server_receive(
    handle: *mut ServerHandle,
    event: *mut LsAudioQuicEvent,
    payload: *mut u8,
    payload_capacity: u32,
    timeout_ms: u32,
) -> i32 {
    if handle.is_null() || event.is_null() {
        return -1;
    }
    let server = &*handle;
    let receiver = match server.receiver.lock() {
        Ok(receiver) => receiver,
        Err(_) => return -1,
    };
    let received = receiver.recv_timeout(Duration::from_millis(timeout_ms as u64));
    let bridge_event = match received {
        Ok(value) => value,
        Err(RecvTimeoutError::Timeout) => return 0,
        Err(RecvTimeoutError::Disconnected) => return -1,
    };

    let target = &mut *event;
    *target = empty_event();
    match bridge_event {
        BridgeEvent::Config(config) => {
            target.kind = EVENT_CONFIG;
            target.sample_rate = config.sample_rate;
            target.bitrate = config.bitrate;
            target.channels = config.channels;
            target.codec = config.codec;
            target.frame_ms = config.frame_ms;
            1
        }
        BridgeEvent::Packet(frame) => {
            target.kind = EVENT_PACKET;
            target.sequence = frame.sequence;
            target.sent_at_epoch_ms = frame.sent_at_epoch_ms;
            target.payload_length = frame.payload.len() as u32;
            if frame.payload.len() > payload_capacity as usize
                || (payload.is_null() && !frame.payload.is_empty())
            {
                return -2;
            }
            if !frame.payload.is_empty() {
                ptr::copy_nonoverlapping(frame.payload.as_ptr(), payload, frame.payload.len());
            }
            1
        }
        BridgeEvent::Disconnected => {
            target.kind = EVENT_DISCONNECTED;
            1
        }
        BridgeEvent::Error(message) => {
            target.kind = EVENT_ERROR;
            set_error(&server.last_error, &message);
            1
        }
    }
}

#[no_mangle]
pub unsafe extern "C" fn ls_audio_quic_server_disconnect(handle: *mut ServerHandle) {
    if let Some(server) = handle.as_ref() {
        server.disconnect_requested.store(true, Ordering::Release);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ls_audio_quic_server_stop(handle: *mut ServerHandle) {
    if let Some(server) = handle.as_ref() {
        server.running.store(false, Ordering::Release);
        server.disconnect_requested.store(true, Ordering::Release);
    }
}

#[no_mangle]
pub unsafe extern "C" fn ls_audio_quic_server_destroy(handle: *mut ServerHandle) {
    if handle.is_null() {
        return;
    }
    let mut server = Box::from_raw(handle);
    server.running.store(false, Ordering::Release);
    server.disconnect_requested.store(true, Ordering::Release);
    if let Some(thread) = server.thread.take() {
        let _ = thread.join();
    }
}

#[no_mangle]
pub unsafe extern "C" fn ls_audio_quic_last_error(
    handle: *mut ServerHandle,
    buffer: *mut c_char,
    capacity: u32,
) -> u32 {
    let source = if let Some(server) = handle.as_ref() {
        &server.last_error
    } else {
        global_error()
    };
    let message = source.lock().map(|value| value.clone()).unwrap_or_default();
    if capacity == 0 || buffer.is_null() {
        return message.len() as u32;
    }
    let bytes = message.as_bytes();
    let copy_length = bytes.len().min(capacity.saturating_sub(1) as usize);
    ptr::copy_nonoverlapping(bytes.as_ptr(), buffer.cast::<u8>(), copy_length);
    *buffer.add(copy_length) = 0;
    copy_length as u32
}

fn run_server_thread(
    port: u16,
    events: Sender<BridgeEvent>,
    ready: mpsc::SyncSender<Result<(), String>>,
    running: Arc<AtomicBool>,
    disconnect_requested: Arc<AtomicBool>,
    last_error: Arc<Mutex<String>>,
) {
    let runtime = match tokio::runtime::Builder::new_multi_thread()
        .worker_threads(2)
        .enable_all()
        .build()
    {
        Ok(runtime) => runtime,
        Err(error) => {
            let message = format!("无法创建 QUIC 运行时：{error}");
            let _ = ready.send(Err(message.clone()));
            set_error(&last_error, &message);
            return;
        }
    };

    runtime.block_on(async move {
        let endpoint = match create_endpoint(port) {
            Ok(endpoint) => endpoint,
            Err(error) => {
                let message = format!("QUIC 监听失败：{error:#}");
                let _ = ready.send(Err(message.clone()));
                set_error(&last_error, &message);
                return;
            }
        };
        let _ = ready.send(Ok(()));

        while running.load(Ordering::Acquire) {
            let incoming = tokio::select! {
                incoming = endpoint.accept() => incoming,
                _ = tokio::time::sleep(Duration::from_millis(100)) => continue,
            };
            let Some(incoming) = incoming else { break };
            disconnect_requested.store(false, Ordering::Release);
            if let Err(error) = handle_connection(
                incoming,
                events.clone(),
                running.clone(),
                disconnect_requested.clone(),
            )
            .await
            {
                let message = format!("QUIC 会话失败：{error:#}");
                set_error(&last_error, &message);
                let _ = events.send(BridgeEvent::Error(message));
            }
            let _ = events.send(BridgeEvent::Disconnected);
        }
        endpoint.close(0_u32.into(), b"server stopped");
        endpoint.wait_idle().await;
    });
}

fn create_endpoint(port: u16) -> Result<quinn::Endpoint> {
    let certified_key = rcgen::generate_simple_self_signed(vec![
        "livesuite.local".to_string(),
        "localhost".to_string(),
    ])?;
    let key = PrivateKeyDer::Pkcs8(PrivatePkcs8KeyDer::from(
        certified_key.signing_key.serialize_der(),
    ));
    let cert: CertificateDer<'static> = certified_key.cert.into();
    let mut crypto = rustls::ServerConfig::builder()
        .with_no_client_auth()
        .with_single_cert(vec![cert], key)?;
    crypto.alpn_protocols = vec![ALPN.to_vec()];
    let mut server_config =
        quinn::ServerConfig::with_crypto(Arc::new(QuicServerConfig::try_from(crypto)?));
    let transport = Arc::get_mut(&mut server_config.transport).expect("unique transport config");
    transport.max_concurrent_bidi_streams(2_u32.into());
    transport.max_concurrent_uni_streams(2_u32.into());
    transport.keep_alive_interval(Some(Duration::from_secs(1)));
    transport.max_idle_timeout(Some(Duration::from_secs(5).try_into()?));
    quinn::Endpoint::server(
        server_config,
        SocketAddr::new(Ipv4Addr::UNSPECIFIED.into(), port),
    )
    .map_err(Into::into)
}

async fn handle_connection(
    incoming: quinn::Incoming,
    events: Sender<BridgeEvent>,
    running: Arc<AtomicBool>,
    disconnect_requested: Arc<AtomicBool>,
) -> Result<()> {
    let connection = incoming.await?;
    let (send, mut receive) = tokio::time::timeout(Duration::from_secs(5), connection.accept_bi())
        .await
        .context("等待音频控制流超时")??;
    let hello = read_control_frame(&mut receive).await?;
    let config = parse_hello(&hello)?;
    events
        .send(BridgeEvent::Config(config))
        .map_err(|_| anyhow!("C++ 接收端已关闭"))?;

    let control_send = Arc::new(tokio::sync::Mutex::new(send));
    let mut ack = Vec::with_capacity(11);
    ack.push(CONTROL_HELLO_ACK);
    push_u64(&mut ack, config.session_id);
    push_u16(&mut ack, u16::MAX);
    write_control_frame(&control_send, &ack).await?;

    let stats = Arc::new(tokio::sync::Mutex::new(SessionStats::new()));
    let control_future = run_control(
        receive,
        control_send.clone(),
        config.session_id,
        events,
        stats.clone(),
    );
    let stats_future = run_stats(control_send, stats);
    let shutdown_future = wait_for_shutdown(running, disconnect_requested);

    tokio::select! {
        result = control_future => result?,
        result = stats_future => result?,
        _ = shutdown_future => {},
        _ = connection.closed() => {},
    }
    connection.close(0_u32.into(), b"audio session ended");
    Ok(())
}

async fn run_control(
    mut receive: quinn::RecvStream,
    send: Arc<tokio::sync::Mutex<quinn::SendStream>>,
    session_id: u64,
    events: Sender<BridgeEvent>,
    stats: Arc<tokio::sync::Mutex<SessionStats>>,
) -> Result<()> {
    loop {
        let data = read_control_frame(&mut receive).await?;
        if data.is_empty() {
            continue;
        }
        match data[0] {
            CONTROL_SYNC_REQUEST => {
                if data.len() != 13 {
                    return Err(anyhow!("时钟同步请求长度无效"));
                }
                let sequence = read_u32(&data, 1)?;
                let t0 = read_i64(&data, 5)?;
                let t1 = epoch_ms();
                let mut response = Vec::with_capacity(29);
                response.push(CONTROL_SYNC_RESPONSE);
                push_u32(&mut response, sequence);
                push_i64(&mut response, t0);
                push_i64(&mut response, t1);
                push_i64(&mut response, epoch_ms());
                write_control_frame(&send, &response).await?;
            }
            CONTROL_SYNC_RESULT => {
                if data.len() != 25 {
                    return Err(anyhow!("时钟同步结果长度无效"));
                }
                let minimum = read_i64(&data, 5)?;
                let maximum = read_i64(&data, 13)?;
                if minimum <= maximum && maximum.saturating_sub(minimum) <= 10_000 {
                    stats.lock().await.clock_bounds = Some((minimum, maximum));
                }
            }
            CONTROL_AUDIO => {
                let frame = parse_audio_stream_frame(&data, session_id)?;
                let received_at = epoch_ms();
                if stats.lock().await.accept(&frame, received_at) {
                    events
                        .send(BridgeEvent::Packet(frame))
                        .map_err(|_| anyhow!("C++ 接收端已关闭"))?;
                }
            }
            CONTROL_STOP => return Ok(()),
            other => return Err(anyhow!("未知控制消息 {other:#x}")),
        }
    }
}

async fn run_stats(
    send: Arc<tokio::sync::Mutex<quinn::SendStream>>,
    stats: Arc<tokio::sync::Mutex<SessionStats>>,
) -> Result<()> {
    let mut interval = tokio::time::interval(Duration::from_millis(250));
    interval.tick().await;
    loop {
        interval.tick().await;
        let payload = stats.lock().await.stats_payload();
        write_control_frame(&send, &payload).await?;
    }
}

async fn wait_for_shutdown(running: Arc<AtomicBool>, disconnect_requested: Arc<AtomicBool>) {
    while running.load(Ordering::Acquire) && !disconnect_requested.load(Ordering::Acquire) {
        tokio::time::sleep(Duration::from_millis(50)).await;
    }
}

fn parse_hello(data: &[u8]) -> Result<AudioConfig> {
    if data.len() != 22 || data[0] != CONTROL_HELLO || data[1] != VERSION {
        return Err(anyhow!("音频握手无效"));
    }
    let config = AudioConfig {
        session_id: read_u64(data, 2)?,
        sample_rate: read_u32(data, 10)?,
        channels: data[14],
        codec: data[15],
        frame_ms: data[16],
        bitrate: read_u32(data, 17)?,
    };
    if !matches!(config.sample_rate, 44_100 | 48_000) {
        return Err(anyhow!("不支持的采样率 {}", config.sample_rate));
    }
    if !matches!(config.channels, 1 | 2) || !matches!(config.codec, 0 | 1) {
        return Err(anyhow!("不支持的音频格式"));
    }
    if config.codec == 1 && !matches!(config.frame_ms, 10 | 20 | 40) {
        return Err(anyhow!("不支持的 Opus 帧长 {}", config.frame_ms));
    }
    Ok(config)
}

fn parse_audio_stream_frame(data: &[u8], expected_session_id: u64) -> Result<AudioFrame> {
    if data.len() < 20 || data[0] != CONTROL_AUDIO {
        return Err(anyhow!("音频 Stream 帧头无效"));
    }
    if read_u64(data, 1)? != expected_session_id {
        return Err(anyhow!("音频 Stream 会话不匹配"));
    }
    let payload_length = read_u16(data, 18)? as usize;
    if payload_length == 0
        || payload_length > MAX_AUDIO_PAYLOAD
        || data.len() != 20 + payload_length
    {
        return Err(anyhow!("音频 Stream 负载长度无效"));
    }
    Ok(AudioFrame {
        sequence: data[9],
        sent_at_epoch_ms: read_i64(data, 10)?,
        payload: data[20..].to_vec(),
    })
}

async fn read_control_frame(receive: &mut quinn::RecvStream) -> Result<Vec<u8>> {
    let length = receive.read_u32().await? as usize;
    if length == 0 || length > MAX_CONTROL_FRAME {
        return Err(anyhow!("控制消息长度无效"));
    }
    let mut data = vec![0_u8; length];
    receive.read_exact(&mut data).await?;
    Ok(data)
}

async fn write_control_frame(
    send: &Arc<tokio::sync::Mutex<quinn::SendStream>>,
    data: &[u8],
) -> Result<()> {
    let mut stream = send.lock().await;
    stream.write_u32(data.len() as u32).await?;
    stream.write_all(data).await?;
    Ok(())
}

fn empty_event() -> LsAudioQuicEvent {
    LsAudioQuicEvent {
        kind: 0,
        sample_rate: 0,
        bitrate: 0,
        sent_at_epoch_ms: 0,
        payload_length: 0,
        sequence: 0,
        channels: 0,
        codec: 0,
        frame_ms: 0,
    }
}

fn sequence_is_newer(last: u8, incoming: u8) -> bool {
    incoming != last && incoming.wrapping_sub(last) < 128
}

fn epoch_ms() -> i64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .unwrap_or_default()
        .as_millis()
        .min(i64::MAX as u128) as i64
}

fn set_error(target: &Mutex<String>, message: &str) {
    if let Ok(mut value) = target.lock() {
        *value = message.to_string();
    }
}

fn read_u16(data: &[u8], offset: usize) -> Result<u16> {
    Ok(u16::from_be_bytes(
        data.get(offset..offset + 2)
            .context("u16 越界")?
            .try_into()?,
    ))
}

fn read_u32(data: &[u8], offset: usize) -> Result<u32> {
    Ok(u32::from_be_bytes(
        data.get(offset..offset + 4)
            .context("u32 越界")?
            .try_into()?,
    ))
}

fn read_u64(data: &[u8], offset: usize) -> Result<u64> {
    Ok(u64::from_be_bytes(
        data.get(offset..offset + 8)
            .context("u64 越界")?
            .try_into()?,
    ))
}

fn read_i64(data: &[u8], offset: usize) -> Result<i64> {
    Ok(i64::from_be_bytes(
        data.get(offset..offset + 8)
            .context("i64 越界")?
            .try_into()?,
    ))
}

fn push_u16(output: &mut Vec<u8>, value: u16) {
    output.extend_from_slice(&value.to_be_bytes());
}

fn push_u32(output: &mut Vec<u8>, value: u32) {
    output.extend_from_slice(&value.to_be_bytes());
}

fn push_i32(output: &mut Vec<u8>, value: i32) {
    output.extend_from_slice(&value.to_be_bytes());
}

fn push_u64(output: &mut Vec<u8>, value: u64) {
    output.extend_from_slice(&value.to_be_bytes());
}

fn push_i64(output: &mut Vec<u8>, value: i64) {
    output.extend_from_slice(&value.to_be_bytes());
}

#[cfg(test)]
mod tests {
    use super::{parse_audio_stream_frame, push_i64, push_u16, push_u64, CONTROL_AUDIO};

    fn packet(session_id: u64, sequence: u8, payload: &[u8]) -> Vec<u8> {
        let mut data = vec![CONTROL_AUDIO];
        push_u64(&mut data, session_id);
        data.push(sequence);
        push_i64(&mut data, 1_234);
        push_u16(&mut data, payload.len() as u16);
        data.extend_from_slice(payload);
        data
    }

    #[test]
    fn parses_reliable_audio_frame() {
        let parsed = parse_audio_stream_frame(&packet(7, 42, &[1, 2, 3]), 7).unwrap();
        assert_eq!(parsed.sequence, 42);
        assert_eq!(parsed.sent_at_epoch_ms, 1_234);
        assert_eq!(parsed.payload, vec![1, 2, 3]);
    }

    #[test]
    fn rejects_wrong_session_and_length() {
        assert!(parse_audio_stream_frame(&packet(7, 1, &[9]), 8).is_err());
        let mut malformed = packet(7, 1, &[9]);
        malformed.push(0);
        assert!(parse_audio_stream_frame(&malformed, 7).is_err());
    }
}
