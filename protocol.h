#pragma once
// ============================================================================
// protocol.h — TCP + UDP 双重音频协议定义
// ============================================================================

#include <cstdint>
#include <cstddef>
#include <winsock2.h>

// 辅助：确保 64 位字节序转换宏正确
#if defined(_WIN32) || defined(_WIN64)
#include <stdlib.h>
#define htonll(x) _byteswap_uint64(x)
#define ntohll(x) _byteswap_uint64(x)
#else
#include <arpa/inet.h>
#define htonll(x) htobe64(x)
#define ntohll(x) be64toh(x)
#endif

// TCP 握手包 (客户端 -> 服务端)
#pragma pack(push, 1)
struct TcpConfigPacket {
    uint8_t sampleRateIdx;    // 0x00: 44100, 0x01: 48000
    uint8_t channels;         // 0x01: Mono, 0x02: Stereo
};
#pragma pack(pop)

// TCP 延迟反馈包 (服务端 -> 客户端)
#pragma pack(push, 1)
struct TcpLatencyPacket {
    uint8_t seq;
    uint64_t udpRecvTs; // microseconds, big-endian
    uint64_t serverTs;  // microseconds, big-endian
};
#pragma pack(pop)

// UDP 数据包头 (1 字节序号 + PCM)
struct UdpDataHeader {
    uint8_t seq;
};

// 用于排序区的数据包结构
struct StoredPacket
{
    std::vector<uint8_t> data;
};

inline bool isNewer(uint8_t last, uint8_t incoming)
{
    return incoming != last && uint8_t(incoming - last) < 15;
}
    
inline uint32_t getSampleRate(uint8_t index) {
    switch (index) {
        case 0x00: return 44100;
        case 0x01: return 48000;
        default:   return 0;
    }
}

inline void convertInt16ToFloat32(const int16_t* src, float* dst, size_t sample_count) {
    constexpr float kScale = 1.0f / 32768.0f;
    for (size_t i = 0; i < sample_count; ++i) {
        dst[i] = static_cast<float>(src[i]) * kScale;
    }
}
