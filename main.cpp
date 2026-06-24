// ============================================================================
// main.cpp — UDP 低延迟音频直通服务器
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <winsock2.h>
#include <ws2tcpip.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <vector>
#include <map>
#include <chrono>
#include <thread>
#include <mutex>
#include <atomic>
#include <algorithm>

#include "protocol.h"
#include "ring_buffer.h"
#include "wasapi_renderer.h"
#include "opus_decoder.h"

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

static std::atomic<bool> g_running{true};
static SOCKET g_udpSocket = INVALID_SOCKET;
static SOCKET g_tcpSocket = INVALID_SOCKET;
static SOCKET g_tcpClient = INVALID_SOCKET;   // 当前 TCP 客户端连接，供渲染线程超时时关闭
static uint16_t g_tcpPort = 9000;
static uint16_t g_udpPort = 9000;
static std::mutex g_configMutex;
static bool g_configReady = false;
static uint32_t g_sampleRate = 48000;
static uint8_t g_channels = 2;
static uint8_t g_codec = AUDIO_CODEC_PCM;
static uint8_t g_frameMs = 20;
static uint8_t g_opusBitrateKbps = 32;

// 每个序号最早到达时间（ms），0 表示未到达；由 UDP 主线程写，TCP 线程读
static std::atomic<uint64_t> g_seqTimestamps[256];
// 上次心跳回复时已发送到的区间终点（即本次区间起点）
static std::atomic<uint8_t> g_hbLastEndSeq{0};
// 当前 expectedSeq，由 UDP 主线程维护，供 TCP 线程读取
static std::atomic<uint8_t> g_expectedSeq{0};

static void closeTcpClient()
{
    SOCKET s = g_tcpClient;
    if (s != INVALID_SOCKET)
    {
        shutdown(s, SD_BOTH);
        closesocket(s);
        g_tcpClient = INVALID_SOCKET;
    }
}

static void signalHandler(int sig)
{
    (void)sig;
    printf("\n[System] 收到退出信号，开始清理...\n");
    g_running = false;
    if (g_udpSocket != INVALID_SOCKET)
    {
        shutdown(g_udpSocket, SD_BOTH);
        closesocket(g_udpSocket);
        g_udpSocket = INVALID_SOCKET;
    }
    if (g_tcpSocket != INVALID_SOCKET)
    {
        shutdown(g_tcpSocket, SD_BOTH);
        closesocket(g_tcpSocket);
        g_tcpSocket = INVALID_SOCKET;
    }
}

#pragma pack(push, 1)
struct HeartbeatRequest
{
    uint8_t seq;
    uint64_t clientTimestamp;
};
#pragma pack(pop)

static bool recvAll(SOCKET socket, void *buffer, int length)
{
    char *cursor = static_cast<char *>(buffer);
    int total = 0;
    while (total < length)
    {
        int received = recv(socket, cursor + total, length - total, 0);
        if (received <= 0)
            return false;
        total += received;
    }
    return true;
}

void tcpHandler()
{
    g_tcpSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {AF_INET};
    addr.sin_port = htons(g_tcpPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(g_tcpSocket, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        printf("[TCP] 绑定端口 %u 失败，端口可能被占用 (错误码: %d)\n", g_tcpPort, WSAGetLastError());
        closesocket(g_tcpSocket);
        WSACleanup();
        exit(1);
    }
    listen(g_tcpSocket, 1);

    while (g_running)
    {
        sockaddr_in clientAddr;
        int clientLen = sizeof(clientAddr);
        SOCKET client = accept(g_tcpSocket, (sockaddr *)&clientAddr, &clientLen);
        if (client == INVALID_SOCKET)
            continue;

        printf("[TCP] 客户端已连接\n");
        g_tcpClient = client;

        TcpConfigPacket config;
        bool configOk = recvAll(client, &config, (int)sizeof(config));

        if (configOk)
        {
            uint32_t sampleRate = getSampleRate(config.sampleRateIdx);
            bool supported = true;
            if (config.version != PROTOCOL_VERSION)
            {
                printf("[TCP] 不支持的协议版本: %u\n", config.version);
                supported = false;
            }
            if (sampleRate == 0)
            {
                printf("[TCP] 不支持的采样率索引: %u\n", config.sampleRateIdx);
                supported = false;
            }
            if (config.channels != 1 && config.channels != 2)
            {
                printf("[TCP] 不支持的声道数: %u\n", config.channels);
                supported = false;
            }
            if (config.codec != AUDIO_CODEC_PCM && config.codec != AUDIO_CODEC_OPUS)
            {
                printf("[TCP] 不支持的编码格式: %u\n", config.codec);
                supported = false;
            }
            if (config.codec == AUDIO_CODEC_OPUS &&
                config.frameMs != 10 && config.frameMs != 20 && config.frameMs != 40)
            {
                printf("[TCP] 不支持的 Opus 帧长: %u ms\n", config.frameMs);
                supported = false;
            }

            if (!supported)
            {
                shutdown(client, SD_BOTH);
                closesocket(client);
                g_tcpClient = INVALID_SOCKET;
                continue;
            }

            printf("[TCP] 收到配置: 版本 %u, %u Hz, 声道: %u, 编码: %s, 帧长: %u ms, 码率: %u kbps\n",
                   config.version, sampleRate, config.channels,
                   config.codec == AUDIO_CODEC_PCM ? "PCM" : "Opus",
                   config.frameMs, config.opusBitrateKbps);
            {
                std::lock_guard<std::mutex> lock(g_configMutex);
                g_sampleRate = sampleRate;
                g_channels = config.channels;
                g_codec = config.codec;
                g_frameMs = config.frameMs;
                g_opusBitrateKbps = config.opusBitrateKbps;
                g_configReady = true;
            }
            send(client, "\0", 1, 0); // 确认握手

            char hbBuf[128];
            int bytesRead;
            while (g_running && (bytesRead = recv(client, hbBuf, sizeof(hbBuf), 0)) > 0)
            {
                if (bytesRead >= (int)sizeof(HeartbeatRequest))
                {
                    HeartbeatRequest *req = (HeartbeatRequest *)hbBuf;
                    (void)req;
                    uint8_t endSeq = g_expectedSeq.load(std::memory_order_relaxed);
                    uint8_t startSeq = g_hbLastEndSeq.load(std::memory_order_relaxed);

                    // 包头 2 字节（大端序总长度）+ 最多 256 个序号每条 9 字节 + 末尾 8 字节服务端时间戳
                    uint8_t response[2 + 256 * 9 + 8];
                    int offset = 2; // 预留包头

                    uint8_t cur = startSeq;
                    while (cur != endSeq)
                    {
                        uint64_t ts = g_seqTimestamps[cur].load(std::memory_order_relaxed);
                        response[offset++] = cur;
                        for (int i = 0; i < 8; ++i)
                            response[offset++] = (uint8_t)(ts >> ((7 - i) * 8));
                        ++cur; // uint8_t 自动回绕
                    }

                    uint64_t sTs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
                    for (int i = 0; i < 8; ++i)
                        response[offset++] = (uint8_t)(sTs >> ((7 - i) * 8));

                    // 回填包头：总长度（含头部 2 字节）
                    response[0] = (uint8_t)(offset >> 8);
                    response[1] = (uint8_t)(offset);

                    g_hbLastEndSeq.store(endSeq, std::memory_order_relaxed);
                    send(client, (char *)response, offset, 0);
                }
            }
        }
        printf("[TCP] 客户端断开\n");
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            g_configReady = false;
        }
        // 关闭 UDP socket 以唤醒主线程阻塞的 recvfrom (同 Ctrl+C 机制)
        if (g_udpSocket != INVALID_SOCKET)
        {
            shutdown(g_udpSocket, SD_BOTH);
            closesocket(g_udpSocket);
            g_udpSocket = INVALID_SOCKET;
        }
        closesocket(client);
        g_tcpClient = INVALID_SOCKET;
    }
}

int main(int argc, char *argv[])
{
    SetConsoleOutputCP(CP_UTF8);
    setvbuf(stdout, NULL, _IONBF, 0);

    bool discardOutOfOrder = false;
    uint32_t dropBaselineMs = 0;
    uint32_t protectMs = 50;
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--discard-out-of-order") == 0) {
            discardOutOfOrder = true;
            printf("[System] 已启用丢弃乱序包模式\n");
        } else if (strcmp(argv[i], "--drop-baseline-duration-ms") == 0 && i + 1 < argc) {
            dropBaselineMs = (uint32_t)atoi(argv[++i]);
            printf("[System] 丢弃率基准延迟: %u ms\n", dropBaselineMs);
        } else if (strcmp(argv[i], "--protect-ms") == 0 && i + 1 < argc) {
            protectMs = (uint32_t)atoi(argv[++i]);
            printf("[System] 丢弃保护延迟: %u ms\n", protectMs);
        } else if (strcmp(argv[i], "--tcp") == 0 && i + 1 < argc) {
            g_tcpPort = (uint16_t)atoi(argv[++i]);
            printf("[System] TCP 端口已设置为 %u\n", g_tcpPort);
        } else if (strcmp(argv[i], "--udp") == 0 && i + 1 < argc) {
            g_udpPort = (uint16_t)atoi(argv[++i]);
            printf("[System] UDP 端口已设置为 %u\n", g_udpPort);
        }
    }

    signal(SIGINT, signalHandler);
    signal(SIGTERM, signalHandler);

    WSADATA wsaData;
    WSAStartup(MAKEWORD(2, 2), &wsaData);
    std::thread tcpThread(tcpHandler);

    g_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    sockaddr_in addr = {AF_INET};
    addr.sin_port = htons(g_udpPort);
    if (bind(g_udpSocket, (sockaddr *)&addr, sizeof(addr)) == SOCKET_ERROR)
    {
        printf("[UDP] 绑定端口 %u 失败，端口可能被占用 (错误码: %d)\n", g_udpPort, WSAGetLastError());
        closesocket(g_udpSocket);
        closesocket(g_tcpSocket);
        WSACleanup();
        exit(1);
    }

    WasapiRenderer renderer;
    OpusDecoderRuntime opusDecoder;
    RingBuffer *ringBuffer = nullptr;
    bool initialized = false;
    uint8_t activeCodec = AUDIO_CODEC_PCM;
    
    // 重排序相关
    StoredPacket* sortingArea[256] = {nullptr};
    uint8_t expectedSeq = 0;

    printf("[Server] 等待客户端配置...\n");

    // 渲染线程超时 → 关闭 TCP 客户端连接 → tcpHandler 检测到断开 → 走清理路径
    renderer.onFatalTimeout = []() {
        printf("[WASAPI] 渲染线程超时，关闭 TCP 客户端连接以触发清理\n");
        closeTcpClient();
    };

    uint8_t recvBuf[65536];
    sockaddr_in clientAddr;
    int clientLen = sizeof(clientAddr);
    std::vector<int16_t> decodedPcm;

    auto decodeAudioPayload = [&](const uint8_t* audioData, size_t audioLen, std::vector<int16_t>& pcm) {
        pcm.clear();
        if (activeCodec == AUDIO_CODEC_PCM)
        {
            size_t sampleCount = audioLen / sizeof(int16_t);
            const int16_t* samples = reinterpret_cast<const int16_t*>(audioData);
            pcm.assign(samples, samples + sampleCount);
            return true;
        }

        return opusDecoder.decode(audioData, audioLen, pcm);
    };

    auto writePcm = [&](const std::vector<int16_t>& pcm) {
        if (!pcm.empty())
            ringBuffer->write(pcm.data(), pcm.size());
    };

    while (g_running)
    {
        bool configReady;
        uint32_t sr;
        uint8_t ch;
        uint8_t codec;
        uint8_t frameMs;
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            configReady = g_configReady;
            sr = g_sampleRate;
            ch = g_channels;
            codec = g_codec;
            frameMs = g_frameMs;
        }

        if (configReady && !initialized)
        {
            printf("[Server] 正在初始化渲染器...\n");
            ringBuffer = new RingBuffer((size_t)sr * ch * 2);
            bool decoderReady = true;
            if (codec == AUDIO_CODEC_OPUS)
                decoderReady = opusDecoder.init(sr, ch, frameMs);

            if (decoderReady && renderer.init(sr, ch, ringBuffer))
            {
                renderer.start();
                renderer.setDropBaseline(dropBaselineMs);
                renderer.setProtect(protectMs);
                initialized = true;
                activeCodec = codec;
                expectedSeq = 0;
                renderer.setBufferLow(false);
                for(int i=0; i<256; ++i) { delete sortingArea[i]; sortingArea[i] = nullptr; }
                for (int i = 0; i < 256; ++i) g_seqTimestamps[i].store(0, std::memory_order_relaxed);
                g_hbLastEndSeq.store(0, std::memory_order_relaxed);
                g_expectedSeq.store(0, std::memory_order_relaxed);
                printf("[Server] 渲染器初始化完成\n");
            }
            else
            {
                printf("[Server] 渲染器初始化失败！\n");
                opusDecoder.reset();
                delete ringBuffer;
                ringBuffer = nullptr;
                closeTcpClient();
                Sleep(1000);
            }
        }
        else if (!configReady && initialized)
        {
            printf("[Server] TCP 连接断开，停止渲染\n");
            renderer.stop();
            opusDecoder.reset();
            delete ringBuffer;
            ringBuffer = nullptr;
            initialized = false;
        }

        if (!initialized)
        {
            // 若 UDP socket 已被关闭（TCP 断开导致），重新创建以等待新客户端
            if (g_udpSocket == INVALID_SOCKET)
            {
                g_udpSocket = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
                if (g_udpSocket != INVALID_SOCKET)
                    bind(g_udpSocket, (sockaddr *)&addr, sizeof(addr));
            }
            Sleep(100);
            continue;
        }

        // --- 等待数据（阻塞直到有数据到达） ---
        int bytesReceived = recvfrom(g_udpSocket, (char *)recvBuf, sizeof(recvBuf), 0, (sockaddr *)&clientAddr, &clientLen);

        if (bytesReceived <= 0)
            continue;
        
        uint8_t seq = recvBuf[0];
        const uint8_t* audioData = recvBuf + 1;
        size_t audioLen = bytesReceived - 1;

        if (discardOutOfOrder) {
            if (isNewer(expectedSeq - 1, seq)) {
                if (seq != expectedSeq) {
                    printf("[UDP] [DiscardMode] 跳包: 期望 %u, 收到 %u, 跳过区间 [%u, %u]\n", expectedSeq, seq, expectedSeq, (uint8_t)(seq - 1));
                }
                if (decodeAudioPayload(audioData, audioLen, decodedPcm))
                    writePcm(decodedPcm);
                expectedSeq = seq + 1;
            }
        } else {
            if (isNewer(expectedSeq - 1, seq)) {
                if (seq == expectedSeq) {
                    if (decodeAudioPayload(audioData, audioLen, decodedPcm))
                    {
                        writePcm(decodedPcm);
                        renderer.setBufferLow(false);
                        expectedSeq++;
                    }
                } else {
                    if (uint8_t(seq - expectedSeq) >= 4 || renderer.isBufferLow()) {
                         printf("[UDP] [Reorder] 强制跳过: 期望 %u, 收到 %u, 跳过%u\n", expectedSeq, seq, expectedSeq);
                         // 强制跳过，直接将期望更新到 seq
                         expectedSeq++;
                    }
                    if (!sortingArea[seq]) {
                        if (decodeAudioPayload(audioData, audioLen, decodedPcm))
                        {
                            printf("[UDP] [Reorder] 加入排序区: 序号 %u\n", seq);
                            sortingArea[seq] = new StoredPacket{decodedPcm};
                        }
                    }
                }
                
                // 核心修复：更新序号后检查并释放排序区
                while (sortingArea[expectedSeq]) {
                    StoredPacket* p = sortingArea[expectedSeq];
                    printf("[UDP] [Reorder] 释放排序包: 序号 %u\n", expectedSeq);
                    writePcm(p->pcm);
                    delete p;
                    sortingArea[expectedSeq] = nullptr;
                    expectedSeq++;
                }
            }
        }

        {
            uint64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(
                               std::chrono::system_clock::now().time_since_epoch())
                               .count();
            uint64_t prev = g_seqTimestamps[seq].load(std::memory_order_relaxed);
            if (prev == 0 || now - prev > 300)
                g_seqTimestamps[seq].store(now, std::memory_order_relaxed);
        }
        g_expectedSeq.store(expectedSeq, std::memory_order_relaxed);
    }

    if (initialized)
    {
        renderer.stop();
        delete ringBuffer;
    }
    g_running = false;
    tcpThread.join();
    if (g_udpSocket != INVALID_SOCKET)
        closesocket(g_udpSocket);
    WSACleanup();
    return 0;
}
