// ============================================================================
// opus_decoder.cpp — Opus 运行时解码封装
// ============================================================================

#include "opus_decoder.h"
#include <opus.h>
#include <cstdio>

OpusDecoderRuntime::~OpusDecoderRuntime()
{
    reset();
}

bool OpusDecoderRuntime::init(uint32_t sampleRate, uint8_t channels, uint8_t frameMs)
{
    reset();

    int error = 0;
    decoder_ = opus_decoder_create((int)sampleRate, (int)channels, &error);
    if (!decoder_ || error != 0)
    {
        printf("[Opus] 创建解码器失败: %d\n", error);
        decoder_ = nullptr;
        return false;
    }

    sampleRate_ = sampleRate;
    channels_ = channels;
    frameSamples_ = (int)(sampleRate * frameMs / 1000);

    printf("[Opus] 解码器初始化成功: %u Hz, 声道: %u, 帧长: %u ms\n", sampleRate_, channels_, frameMs);
    return true;
}

void OpusDecoderRuntime::reset()
{
    if (decoder_)
    {
        opus_decoder_destroy(decoder_);
        decoder_ = nullptr;
    }
    sampleRate_ = 0;
    channels_ = 0;
    frameSamples_ = 0;
}

bool OpusDecoderRuntime::decode(const uint8_t* data, size_t length, std::vector<int16_t>& pcm)
{
    if (!decoder_ || !data || length == 0)
        return false;

    pcm.resize((size_t)frameSamples_ * channels_);
    int decodedFrames = opus_decode(decoder_, data, (int)length, pcm.data(), frameSamples_, 0);
    if (decodedFrames < 0)
    {
        printf("[Opus] 解码失败: %d\n", decodedFrames);
        pcm.clear();
        return false;
    }

    pcm.resize((size_t)decodedFrames * channels_);
    return true;
}
