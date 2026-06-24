#pragma once
// ============================================================================
// opus_decoder.h — Opus 运行时解码封装
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <cstdint>
#include <vector>

struct OpusDecoder;

class OpusDecoderRuntime
{
public:
    OpusDecoderRuntime() = default;
    ~OpusDecoderRuntime();

    OpusDecoderRuntime(const OpusDecoderRuntime&) = delete;
    OpusDecoderRuntime& operator=(const OpusDecoderRuntime&) = delete;

    bool init(uint32_t sampleRate, uint8_t channels, uint8_t frameMs);
    void reset();
    bool decode(const uint8_t* data, size_t length, std::vector<int16_t>& pcm);

private:
    OpusDecoder* decoder_ = nullptr;
    uint32_t sampleRate_ = 0;
    uint8_t channels_ = 0;
    int maxFrameSamples_ = 0;
};
