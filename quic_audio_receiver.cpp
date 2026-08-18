#include "quic_audio_receiver.h"

#include <vector>

QuicAudioReceiver::~QuicAudioReceiver()
{
    stop();
}

bool QuicAudioReceiver::start(uint16_t port)
{
    stop();
    if (!loadLibrary())
        return false;

    handle_ = startFn_(port);
    if (!handle_)
        return false;
    return true;
}

int QuicAudioReceiver::receive(
    QuicAudioEvent &event,
    uint8_t *payload,
    uint32_t capacity,
    uint32_t timeoutMs)
{
    if (!handle_ || !receiveFn_)
        return -1;
    return receiveFn_(handle_, &event, payload, capacity, timeoutMs);
}

void QuicAudioReceiver::disconnectClient()
{
    if (handle_ && disconnectFn_)
        disconnectFn_(handle_);
}

void QuicAudioReceiver::stop()
{
    if (handle_)
    {
        if (stopFn_)
            stopFn_(handle_);
        if (destroyFn_)
            destroyFn_(handle_);
        handle_ = nullptr;
    }
    unloadLibrary();
}

std::string QuicAudioReceiver::lastError() const
{
    if (!lastErrorFn_)
        return "无法加载 livesuite_audio_quic.dll";
    std::vector<char> buffer(2048, 0);
    lastErrorFn_(handle_, buffer.data(), (uint32_t)buffer.size());
    return std::string(buffer.data());
}

bool QuicAudioReceiver::loadLibrary()
{
    wchar_t executablePath[MAX_PATH] = {};
    DWORD pathLength = GetModuleFileNameW(nullptr, executablePath, MAX_PATH);
    if (pathLength > 0 && pathLength < MAX_PATH)
    {
        wchar_t *lastSeparator = wcsrchr(executablePath, L'\\');
        if (lastSeparator)
        {
            *(lastSeparator + 1) = L'\0';
            wcscat_s(executablePath, L"livesuite_audio_quic.dll");
            module_ = LoadLibraryW(executablePath);
        }
    }
    if (!module_)
        module_ = LoadLibraryW(L"livesuite_audio_quic.dll");
    if (!module_)
        return false;

    startFn_ = reinterpret_cast<StartFn>(GetProcAddress(module_, "ls_audio_quic_server_start"));
    receiveFn_ = reinterpret_cast<ReceiveFn>(GetProcAddress(module_, "ls_audio_quic_server_receive"));
    disconnectFn_ = reinterpret_cast<DisconnectFn>(GetProcAddress(module_, "ls_audio_quic_server_disconnect"));
    stopFn_ = reinterpret_cast<StopFn>(GetProcAddress(module_, "ls_audio_quic_server_stop"));
    destroyFn_ = reinterpret_cast<DestroyFn>(GetProcAddress(module_, "ls_audio_quic_server_destroy"));
    lastErrorFn_ = reinterpret_cast<LastErrorFn>(GetProcAddress(module_, "ls_audio_quic_last_error"));
    if (!startFn_ || !receiveFn_ || !disconnectFn_ || !stopFn_ || !destroyFn_ || !lastErrorFn_)
    {
        unloadLibrary();
        return false;
    }
    return true;
}

void QuicAudioReceiver::unloadLibrary()
{
    startFn_ = nullptr;
    receiveFn_ = nullptr;
    disconnectFn_ = nullptr;
    stopFn_ = nullptr;
    destroyFn_ = nullptr;
    lastErrorFn_ = nullptr;
    if (module_)
    {
        FreeLibrary(module_);
        module_ = nullptr;
    }
}
