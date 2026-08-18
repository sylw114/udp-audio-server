#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <string>

struct QuicAudioEvent
{
    uint32_t kind;
    uint32_t sampleRate;
    uint32_t bitrate;
    int64_t sentAtEpochMs;
    uint32_t payloadLength;
    uint8_t sequence;
    uint8_t channels;
    uint8_t codec;
    uint8_t frameMs;
};

static_assert(sizeof(QuicAudioEvent) == 32, "Rust/C++ QUIC 事件布局不一致");

enum QuicAudioEventKind : uint32_t
{
    QUIC_AUDIO_EVENT_CONFIG = 1,
    QUIC_AUDIO_EVENT_PACKET = 2,
    QUIC_AUDIO_EVENT_DISCONNECTED = 3,
    QUIC_AUDIO_EVENT_ERROR = 4,
};

class QuicAudioReceiver
{
public:
    QuicAudioReceiver() = default;
    ~QuicAudioReceiver();

    QuicAudioReceiver(const QuicAudioReceiver &) = delete;
    QuicAudioReceiver &operator=(const QuicAudioReceiver &) = delete;

    bool start(uint16_t port);
    int receive(QuicAudioEvent &event, uint8_t *payload, uint32_t capacity, uint32_t timeoutMs);
    void disconnectClient();
    void stop();
    std::string lastError() const;
    bool isStarted() const { return handle_ != nullptr; }

private:
    using StartFn = void *(__cdecl *)(uint16_t);
    using ReceiveFn = int(__cdecl *)(void *, QuicAudioEvent *, uint8_t *, uint32_t, uint32_t);
    using DisconnectFn = void(__cdecl *)(void *);
    using StopFn = void(__cdecl *)(void *);
    using DestroyFn = void(__cdecl *)(void *);
    using LastErrorFn = uint32_t(__cdecl *)(void *, char *, uint32_t);

    bool loadLibrary();
    void unloadLibrary();

    HMODULE module_ = nullptr;
    void *handle_ = nullptr;
    StartFn startFn_ = nullptr;
    ReceiveFn receiveFn_ = nullptr;
    DisconnectFn disconnectFn_ = nullptr;
    StopFn stopFn_ = nullptr;
    DestroyFn destroyFn_ = nullptr;
    LastErrorFn lastErrorFn_ = nullptr;
};
