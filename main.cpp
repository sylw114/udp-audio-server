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

static std::atomic<uint8_t> g_lastUdpSeq{0};
static std::atomic<uint64_t> g_lastUdpTimestamp{0};

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

void tcpHandler()
{
    g_tcpSocket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in addr = {AF_INET};
    addr.sin_port = htons(g_tcpPort);
    addr.sin_addr.s_addr = INADDR_ANY;
    bind(g_tcpSocket, (sockaddr *)&addr, sizeof(addr));
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
        int received = recv(client, (char *)&config, sizeof(config), 0);

        if (received == sizeof(config))
        {
            printf("[TCP] 收到配置: %u Hz, 声道: %u\n", getSampleRate(config.sampleRateIdx), config.channels);
            {
                std::lock_guard<std::mutex> lock(g_configMutex);
                g_sampleRate = getSampleRate(config.sampleRateIdx);
                g_channels = config.channels;
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

                    uint8_t response[17];

                    uint8_t lastSeq = g_lastUdpSeq.load(std::memory_order_relaxed);
                    uint64_t lastUdpTs = g_lastUdpTimestamp.load(std::memory_order_relaxed);

                    response[0] = lastSeq;

                    // 2. UDP包接收时间戳 (8B, Big Endian)
                    for (int i = 0; i < 8; ++i)
                    {
                        response[1 + i] = (uint8_t)(lastUdpTs >> ((7 - i) * 8));
                    }

                    // 3. ServerTimestamp (8B, Big Endian)
                    uint64_t sTs = std::chrono::duration_cast<std::chrono::milliseconds>(
                                       std::chrono::system_clock::now().time_since_epoch())
                                       .count();
                    for (int i = 0; i < 8; ++i)
                    {
                        response[9 + i] = (uint8_t)(sTs >> ((7 - i) * 8));
                    }

                    send(client, (char *)response, sizeof(response), 0);
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
    for (int i = 1; i < argc; ++i) {
        if (strcmp(argv[i], "--discard-out-of-order") == 0) {
            discardOutOfOrder = true;
            printf("[System] 已启用丢弃乱序包模式\n");
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
    bind(g_udpSocket, (sockaddr *)&addr, sizeof(addr));

    WasapiRenderer renderer;
    RingBuffer *ringBuffer = nullptr;
    bool initialized = false;
    
    // 重排序相关
    StoredPacket* sortingArea[256] = {nullptr};
    uint8_t expectedSeq = 0;

    printf("[Server] 等待客户端配置...\n");

    // 渲染线程超时 → 关闭 TCP 客户端连接 → tcpHandler 检测到断开 → 走清理路径
    renderer.onFatalTimeout = []() {
        printf("[WASAPI] 渲染线程超时，关闭 TCP 客户端连接以触发清理\n");
        SOCKET s = g_tcpClient;
        if (s != INVALID_SOCKET)
        {
            shutdown(s, SD_BOTH);
            closesocket(s);
            g_tcpClient = INVALID_SOCKET;
        }
    };

    uint8_t recvBuf[65536];
    sockaddr_in clientAddr;
    int clientLen = sizeof(clientAddr);

    while (g_running)
    {
        bool configReady;
        uint32_t sr;
        uint8_t ch;
        {
            std::lock_guard<std::mutex> lock(g_configMutex);
            configReady = g_configReady;
            sr = g_sampleRate;
            ch = g_channels;
        }

        if (configReady && !initialized)
        {
            printf("[Server] 正在初始化渲染器...\n");
            ringBuffer = new RingBuffer((size_t)sr * ch * 2);
            if (renderer.init(sr, ch, ringBuffer))
            {
                renderer.start();
                initialized = true;
                expectedSeq = 0;
                renderer.setBufferLow(false);
                for(int i=0; i<256; ++i) { delete sortingArea[i]; sortingArea[i] = nullptr; }
                printf("[Server] 渲染器初始化完成\n");
            }
            else
            {
                printf("[Server] 渲染器初始化失败！\n");
                delete ringBuffer;
                ringBuffer = nullptr;
                Sleep(1000);
            }
        }
        else if (!configReady && initialized)
        {
            printf("[Server] TCP 连接断开，停止渲染\n");
            renderer.stop();
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
        const uint8_t* pcmData = recvBuf + 1;
        size_t pcmLen = bytesReceived - 1;

        if (discardOutOfOrder) {
            if (isNewer(expectedSeq - 1, seq)) {
                if (seq != expectedSeq) {
                    printf("[UDP] [DiscardMode] 跳包: 期望 %u, 收到 %u, 跳过区间 [%u, %u]\n", expectedSeq, seq, expectedSeq, (uint8_t)(seq - 1));
                }
                ringBuffer->write(reinterpret_cast<const int16_t*>(pcmData), pcmLen / sizeof(int16_t));
                expectedSeq = seq + 1;
            }
        } else {
            if (isNewer(expectedSeq - 1, seq)) {
                if (seq == expectedSeq) {
                    ringBuffer->write(reinterpret_cast<const int16_t*>(pcmData), pcmLen / sizeof(int16_t));
                    renderer.setBufferLow(false);
                    expectedSeq++;
                } else {
                    if (uint8_t(seq - expectedSeq) >= 4 || renderer.isBufferLow()) {
                         printf("[UDP] [Reorder] 强制跳过: 期望 %u, 收到 %u, 跳过%u\n", expectedSeq, seq, expectedSeq);
                         // 强制跳过，直接将期望更新到 seq
                         expectedSeq++;
                    }
                    if (!sortingArea[seq]) {
                        printf("[UDP] [Reorder] 加入排序区: 序号 %u\n", seq);
                        sortingArea[seq] = new StoredPacket{std::vector<uint8_t>(pcmData, pcmData + pcmLen)};
                    }
                }
                
                // 核心修复：更新序号后检查并释放排序区
                while (sortingArea[expectedSeq]) {
                    StoredPacket* p = sortingArea[expectedSeq];
                    printf("[UDP] [Reorder] 释放排序包: 序号 %u\n", expectedSeq);
                    ringBuffer->write(reinterpret_cast<const int16_t*>(p->data.data()), p->data.size() / sizeof(int16_t));
                    delete p;
                    sortingArea[expectedSeq] = nullptr;
                    expectedSeq++;
                }
            }
        }

        g_lastUdpSeq.store(seq, std::memory_order_relaxed);
        g_lastUdpTimestamp.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::system_clock::now().time_since_epoch())
                .count(),
            std::memory_order_relaxed);
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
