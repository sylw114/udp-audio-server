#pragma once
// ============================================================================
// wasapi_renderer.h — WASAPI 事件驱动音频渲染器
// ============================================================================

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <avrt.h>
#include <cstdio>
#include "ring_buffer.h"
#include <atomic>
#include <thread>
#include <vector>
#include <functional>

class WasapiRenderer {
public:
    WasapiRenderer() : hRelinkDll_(nullptr) {
        // 尝试可选加载 DLL
        hRelinkDll_ = LoadLibraryW(L"wasapi_relink.dll");
        if (hRelinkDll_) {
            printf("[WASAPI] 成功加载 wasapi_relink.dll\n");
        } else {
            printf("[WASAPI] 未能加载 wasapi_relink.dll，将使用系统默认 WASAPI 模式\n");
        }
    }
    
    ~WasapiRenderer() {
        stop();
        if (hRelinkDll_) FreeLibrary(hRelinkDll_);
    }

    // 禁止拷贝...
    WasapiRenderer(const WasapiRenderer&) = delete;
    WasapiRenderer& operator=(const WasapiRenderer&) = delete;

    bool init(uint32_t sampleRate, uint8_t channels, RingBuffer* ringBuffer);
    bool start();
    void stop();

    bool isRunning() const { return running_.load(std::memory_order_relaxed); }
    bool isBufferLow() const { return bufferLow_.load(std::memory_order_relaxed); }
    void setBufferLow(bool low) { bufferLow_.store(low, std::memory_order_relaxed); }
    void setDropBaseline(uint32_t ms) { dropBaselineDurationMs_.store(ms, std::memory_order_relaxed); }
    void setProtect(uint32_t ms) { protectMs_.store(ms, std::memory_order_relaxed); }

    // 渲染线程致命超时回调（由主线程设置，触发 TCP 断连）
    std::function<void()> onFatalTimeout;

private:
    void renderThread();

    HMODULE                 hRelinkDll_     = nullptr;
    IMMDeviceEnumerator*    pEnumerator_    = nullptr;
    IMMDevice*              pDevice_        = nullptr;
    IAudioClient*           pAudioClient_   = nullptr;
    IAudioRenderClient*     pRenderClient_  = nullptr;
    HANDLE                  hEvent_         = nullptr;
    HANDLE                  hStopEvent_     = nullptr;
    std::thread             renderThread_;
    std::atomic<bool>       running_{false};
    std::atomic<bool>       bufferLow_{false};

    uint32_t                sampleRate_     = 0;
    uint8_t                 channels_       = 0;
    uint32_t                bufferFrames_   = 0;
    WAVEFORMATEX*           pWaveFormat_    = nullptr;

    RingBuffer*             ringBuffer_     = nullptr;
    int                     renderCount_    = 0;
    int                     silenceFillCount_ = 0;
    std::atomic<uint32_t>   dropBaselineDurationMs_{0};
    std::atomic<uint32_t>   protectMs_{50};
    double                  dropAccum_      = 0.0;
};
