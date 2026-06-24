// ============================================================================
// wasapi_renderer.cpp — WASAPI 事件驱动音频渲染器实现
// ============================================================================

#include "wasapi_renderer.h"
#include "protocol.h"
#include <functiondiscoverykeys_devpkey.h>
#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>

#define SAFE_RELEASE(p)     \
    do                      \
    {                       \
        if ((p))            \
        {                   \
            (p)->Release(); \
            (p) = nullptr;  \
        }                   \
    } while (0)

bool WasapiRenderer::init(uint32_t sampleRate, uint8_t channels, RingBuffer *ringBuffer)
{
    HRESULT hr;
    sampleRate_ = sampleRate;
    channels_ = channels;
    ringBuffer_ = ringBuffer;
    lastOutputSamples_.assign(channels, 0);
    recoveringFromSilence_ = false;

    hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    if (FAILED(hr) && hr != S_FALSE && hr != RPC_E_CHANGED_MODE)
    {
        printf("[WASAPI] COM 初始化失败: 0x%08lX\n", hr);
        return false;
    }

    hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, __uuidof(IMMDeviceEnumerator), (void **)&pEnumerator_);
    if (FAILED(hr))
    {
        printf("[WASAPI] 创建设备枚举器失败: 0x%08lX\n", hr);
        return false;
    }

    hr = pEnumerator_->GetDefaultAudioEndpoint(eRender, eConsole, &pDevice_);
    if (FAILED(hr))
    {
        printf("[WASAPI] 获取默认音频设备失败: 0x%08lX\n", hr);
        return false;
    }

    hr = pDevice_->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, (void **)&pAudioClient_);
    if (FAILED(hr))
    {
        printf("[WASAPI] 激活 IAudioClient 失败: 0x%08lX\n", hr);
        return false;
    }

    // 构建目标 PCM 格式（PCM 16-bit, 请求的采样率/声道数）
    WAVEFORMATEX wfx = {0};
    wfx.wFormatTag = WAVE_FORMAT_PCM;
    wfx.nChannels = channels;
    wfx.nSamplesPerSec = sampleRate;
    wfx.wBitsPerSample = 16;
    wfx.nBlockAlign = channels * 2;
    wfx.nAvgBytesPerSec = sampleRate * wfx.nBlockAlign;
    wfx.cbSize = 0;

    // 共享模式 + 自动 PCM 转换 + 默认质量重采样 + 事件回调
    // 这样无论设备原生格式如何，WASAPI 都会帮我们做格式转换和重采样
    DWORD streamFlags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                        AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                        AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

    hr = pAudioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, streamFlags, 100000, 0, &wfx, nullptr);
    if (FAILED(hr))
    {
        printf("[WASAPI] 带自动转换的 Initialize 失败: 0x%08lX, 回退到设备混合格式...\n", hr);

        // 极简回退：旧系统或特殊设备不支持自动转换标志
        WAVEFORMATEX *pMixFormat = nullptr;
        hr = pAudioClient_->GetMixFormat(&pMixFormat);
        if (FAILED(hr))
            return false;
        hr = pAudioClient_->Initialize(AUDCLNT_SHAREMODE_SHARED, AUDCLNT_STREAMFLAGS_EVENTCALLBACK, 100000, 0, pMixFormat, nullptr);
        CoTaskMemFree(pMixFormat);
        if (FAILED(hr))
        {
            printf("[WASAPI] 回退初始化也失败: 0x%08lX\n", hr);
            return false;
        }
    }

    pAudioClient_->GetBufferSize(&bufferFrames_);
    pAudioClient_->GetService(__uuidof(IAudioRenderClient), (void **)&pRenderClient_);

    hEvent_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    pAudioClient_->SetEventHandle(hEvent_);
    hStopEvent_ = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    printf("[WASAPI] 初始化成功, bufferFrames: %u\n", bufferFrames_);
    return true;
}

void WasapiRenderer::fadeInAfterSilence(int16_t* samples, size_t sampleCount)
{
    if (!recoveringFromSilence_ || channels_ == 0 || sampleCount == 0)
        return;

    size_t frameCount = sampleCount / channels_;
    size_t fadeFrames = std::min(frameCount, (size_t)std::max<uint32_t>(1, sampleRate_ / 200));
    for (size_t frame = 0; frame < fadeFrames; ++frame)
    {
        int32_t gain = (int32_t)(frame + 1);
        int32_t denom = (int32_t)fadeFrames;
        for (uint8_t ch = 0; ch < channels_; ++ch)
        {
            size_t index = frame * channels_ + ch;
            samples[index] = (int16_t)((int32_t)samples[index] * gain / denom);
        }
    }

    recoveringFromSilence_ = false;
}

void WasapiRenderer::fillSmoothSilence(int16_t* samples, size_t startSample, size_t totalSamples)
{
    if (channels_ == 0 || startSample >= totalSamples)
        return;

    size_t startFrame = startSample / channels_;
    size_t totalFrames = totalSamples / channels_;
    size_t fillFrames = totalFrames - startFrame;
    size_t fadeFrames = std::min(fillFrames, (size_t)std::max<uint32_t>(1, sampleRate_ / 200));

    for (size_t frame = 0; frame < fillFrames; ++frame)
    {
        int32_t gain = frame < fadeFrames ? (int32_t)(fadeFrames - frame) : 0;
        int32_t denom = (int32_t)fadeFrames;
        for (uint8_t ch = 0; ch < channels_; ++ch)
        {
            int16_t start = 0;
            if (startSample >= channels_)
                start = samples[(startFrame - 1) * channels_ + ch];
            else if (ch < lastOutputSamples_.size())
                start = lastOutputSamples_[ch];

            size_t index = (startFrame + frame) * channels_ + ch;
            samples[index] = frame < fadeFrames ? (int16_t)((int32_t)start * gain / denom) : 0;
        }
    }

    recoveringFromSilence_ = true;
}

void WasapiRenderer::rememberLastSamples(const int16_t* samples, size_t sampleCount)
{
    if (channels_ == 0 || sampleCount < channels_)
        return;

    size_t lastFrame = sampleCount / channels_ - 1;
    if (lastOutputSamples_.size() != channels_)
        lastOutputSamples_.assign(channels_, 0);
    for (uint8_t ch = 0; ch < channels_; ++ch)
        lastOutputSamples_[ch] = samples[lastFrame * channels_ + ch];
}

size_t WasapiRenderer::renderResampled(int16_t* output, size_t outputSamples, double speed)
{
    if (!output || channels_ == 0 || outputSamples < channels_)
        return 0;

    size_t outputFrames = outputSamples / channels_;
    size_t availableFrames = ringBuffer_->availableToRead() / channels_;
    if (outputFrames == 0 || availableFrames == 0)
        return 0;

    size_t targetInputFrames = (size_t)std::ceil((double)outputFrames * speed);
    targetInputFrames = std::clamp<size_t>(targetInputFrames, 1, availableFrames);
    size_t inputSamples = targetInputFrames * channels_;

    if (stretchInputBuf_.size() < inputSamples)
        stretchInputBuf_.resize(inputSamples);
    size_t gotSamples = ringBuffer_->read(stretchInputBuf_.data(), inputSamples);
    size_t gotFrames = gotSamples / channels_;
    if (gotFrames == 0)
        return 0;

    double actualSpeed = (double)gotFrames / (double)outputFrames;
    for (size_t frame = 0; frame < outputFrames; ++frame)
    {
        double srcPos = (double)frame * actualSpeed;
        size_t i0 = (size_t)srcPos;
        if (i0 >= gotFrames)
            i0 = gotFrames - 1;
        size_t i1 = std::min(i0 + 1, gotFrames - 1);
        double frac = srcPos - (double)i0;

        for (uint8_t ch = 0; ch < channels_; ++ch)
        {
            double a = (double)stretchInputBuf_[i0 * channels_ + ch];
            double b = (double)stretchInputBuf_[i1 * channels_ + ch];
            output[frame * channels_ + ch] = (int16_t)std::lrint(a + (b - a) * frac);
        }
    }

    return outputFrames * channels_;
}

size_t WasapiRenderer::bufferedSamples() const
{
    return ringBuffer_ ? ringBuffer_->availableToRead() : 0;
}

bool WasapiRenderer::start()
{
    if (running_.load())
        return false;
    HRESULT hr = pAudioClient_->Start();
    if (FAILED(hr))
    {
        printf("[WASAPI] 启动音频客户端失败: 0x%08lX\n", hr);
        return false;
    }
    running_.store(true, std::memory_order_release);
    renderThread_ = std::thread(&WasapiRenderer::renderThread, this);
    printf("[WASAPI] 渲染线程启动\n");
    return true;
}

void WasapiRenderer::stop()
{
    if (running_.load())
    {
        running_.store(false, std::memory_order_release);
        if (hStopEvent_)
            SetEvent(hStopEvent_);
        if (renderThread_.joinable())
            renderThread_.join();
    }
    if (pAudioClient_)
        pAudioClient_->Stop();
    SAFE_RELEASE(pRenderClient_);
    SAFE_RELEASE(pAudioClient_);
    SAFE_RELEASE(pDevice_);
    SAFE_RELEASE(pEnumerator_);
    if (hEvent_)
    {
        CloseHandle(hEvent_);
        hEvent_ = nullptr;
    }
    if (hStopEvent_)
    {
        CloseHandle(hStopEvent_);
        hStopEvent_ = nullptr;
    }
}

void WasapiRenderer::renderThread()
{
    DWORD taskIndex = 0;
    HANDLE hTask = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIndex);
    HANDLE waitHandles[2] = {hEvent_, hStopEvent_};

    while (running_.load(std::memory_order_relaxed))
    {
        // WASAPI 渲染驱动
        DWORD waitResult = WaitForMultipleObjects(2, waitHandles, FALSE, 2000);
        if (waitResult == WAIT_OBJECT_0 + 1)
            break;
        if (waitResult == WAIT_TIMEOUT)
        {
            printf("[WASAPI] 渲染事件超时(2s)，触发连接断开...\n");
            if (onFatalTimeout)
                onFatalTimeout();
            break;
        }
        if (waitResult != WAIT_OBJECT_0)
            continue;

        UINT32 padding = 0;
        pAudioClient_->GetCurrentPadding(&padding);
        UINT32 framesAvailable = bufferFrames_ - padding;

        if (framesAvailable > 0)
        {
            // // --- 新增：环缓溢出自修复 ---
            // size_t availableSamples = ringBuffer_->availableToRead();
            // size_t thresholdSamples = (size_t)(40 * sampleRate_ / 1000 * channels_ * 2);
            // if (availableSamples > thresholdSamples) {
            //     printf("[WASAPI] 警告: 环缓延迟过高 (可读样本 %zu > 阈值 %zu)，执行重置\n", availableSamples, thresholdSamples);
            //     ringBuffer_->reset();
            //     availableSamples = 0;
            // }
            // // ---------------------------
            // 感觉效果不行啊延迟是变低了稳定性有点烂
            
            // --- 保持音高的倍速延迟控制 ---
            double targetSpeed = 1.0;
            uint32_t baseline = dropBaselineDurationMs_.load(std::memory_order_relaxed);
            size_t bufferedBeforeRead = bufferedSamples();
            if (baseline > 0)
            {
                // 缓存时长 ms = 可读样本数 / (采样率 * 声道数) * 1000
                double cachedMs = (double)bufferedBeforeRead / (sampleRate_ * channels_) * 1000.0;
                double protect = (double)protectMs_.load(std::memory_order_relaxed);
                double x = (cachedMs - protect) / ((double)baseline);
                if (x > 0.0)
                {
                    // 旧逻辑的保留比例是 1 / exp(...)，等效播放速度就是 exp(...)。
                    targetSpeed = std::exp(x * x * x * protect / (double)baseline);
                }
            }
            targetSpeed = std::clamp(targetSpeed, 0.25, 4.0);
            // ------------------------------------

            BYTE *pData = nullptr;
            pRenderClient_->GetBuffer(framesAvailable, &pData);

            size_t needSamples = (size_t)framesAvailable * channels_;
            size_t readSamples = 0;
            if (baseline == 0)
            {
                readSamples = ringBuffer_->read(reinterpret_cast<int16_t *>(pData), needSamples);
            }
            else
            {
                readSamples = renderResampled(reinterpret_cast<int16_t *>(pData), needSamples, targetSpeed);
            }

            if (readSamples > 0)
                fadeInAfterSilence(reinterpret_cast<int16_t *>(pData), readSamples);

            if (readSamples < needSamples)
            {
                fillSmoothSilence(reinterpret_cast<int16_t *>(pData), readSamples, needSamples);
                silenceFillCount_++;
                setBufferLow(true);
                printf("[WASAPI] 填充静音: 需要 %zu 样本, 但只读到 %zu 样本，播放余量 %zu\n", needSamples, readSamples, bufferedSamples());
            }

            rememberLastSamples(reinterpret_cast<int16_t *>(pData), needSamples);

            pRenderClient_->ReleaseBuffer(framesAvailable, 0);

            if ((++renderCount_ % 1000) == 0)
            {
                printf("[WASAPI] 渲染状态: bufferFrames=%u, padding=%u, 填充静音次数=%d, 播放余量=%zu\n", bufferFrames_, padding, silenceFillCount_, bufferedSamples());
            }
        }
    }
    if (hTask)
        AvRevertMmThreadCharacteristics(hTask);
    printf("[WASAPI] 渲染线程停止\n");
}
