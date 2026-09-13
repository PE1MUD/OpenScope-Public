#include "WasapiPcmOutput.h"

#ifdef _WIN32
#include <Windows.h>
#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <Propsys.h>
#endif

#include <algorithm>
#include <chrono>

namespace
{
    constexpr std::size_t kChannels = 2;
    constexpr std::size_t kMaxQueuedFrames = 44100 / 2; // 500 ms hard ceiling.

#ifdef _WIN32
    // kDeviceFriendlyNameKey, defined locally on purpose.
    //
    // The Blackmagic DeckLink SDK ships an old copy of
    // Functiondiscoverykeys_devpkey.h. Because the DeckLink include folder is
    // on OpenScope's include path, including that header by name can shadow
    // the current Windows SDK header and cause hundreds of DEFINE_PROPERTYKEY
    // redefinition errors.
    //
    // {A45C254E-DF1C-4EFD-8020-67D146A850E0}, PID 14
    constexpr PROPERTYKEY kDeviceFriendlyNameKey =
    {
        {
            0xa45c254e,
            0xdf1c,
            0x4efd,
            { 0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0 }
        },
        14
    };

    std::string wideToUtf8(const wchar_t* text)
    {
        if (text == nullptr || *text == L'\0')
        {
            return {};
        }

        const int required =
            WideCharToMultiByte(
                CP_UTF8, 0, text, -1,
                nullptr, 0, nullptr, nullptr);

        if (required <= 1)
        {
            return {};
        }

        std::string result(
            static_cast<std::size_t>(required - 1),
            '\0');

        WideCharToMultiByte(
            CP_UTF8, 0, text, -1,
            result.data(), required,
            nullptr, nullptr);

        return result;
    }

    std::wstring utf8ToWide(const std::string& text)
    {
        if (text.empty())
        {
            return {};
        }

        const int required =
            MultiByteToWideChar(
                CP_UTF8, 0,
                text.c_str(), -1,
                nullptr, 0);

        if (required <= 1)
        {
            return {};
        }

        std::wstring result(
            static_cast<std::size_t>(required - 1),
            L'\0');

        MultiByteToWideChar(
            CP_UTF8, 0,
            text.c_str(), -1,
            result.data(), required);

        return result;
    }

    std::string friendlyName(IMMDevice* device)
    {
        if (device == nullptr)
        {
            return {};
        }

        IPropertyStore* store = nullptr;
        PROPVARIANT value;
        PropVariantInit(&value);

        std::string result;

        if (SUCCEEDED(
                device->OpenPropertyStore(
                    STGM_READ,
                    &store)) &&
            store != nullptr)
        {
            if (SUCCEEDED(
                    store->GetValue(
                        kDeviceFriendlyNameKey,
                        &value)) &&
                value.vt == VT_LPWSTR &&
                value.pwszVal != nullptr)
            {
                result = wideToUtf8(value.pwszVal);
            }

            PropVariantClear(&value);
            store->Release();
        }

        return result;
    }
#endif
}

std::vector<WasapiOutputDevice>
WasapiPcmOutput::enumerateDevices()
{
    std::vector<WasapiOutputDevice> result;

#ifdef _WIN32
    const HRESULT coHr =
        CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED);

    const bool uninitializeCom =
        SUCCEEDED(coHr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;

    HRESULT hr =
        CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator));

    if (SUCCEEDED(hr))
    {
        hr =
            enumerator->EnumAudioEndpoints(
                eRender,
                DEVICE_STATE_ACTIVE,
                &collection);
    }

    if (SUCCEEDED(hr) && collection != nullptr)
    {
        UINT count = 0;
        collection->GetCount(&count);

        for (UINT index = 0; index < count; ++index)
        {
            IMMDevice* device = nullptr;

            if (FAILED(
                    collection->Item(
                        index,
                        &device)) ||
                device == nullptr)
            {
                continue;
            }

            LPWSTR endpointId = nullptr;

            if (SUCCEEDED(
                    device->GetId(
                        &endpointId)) &&
                endpointId != nullptr)
            {
                WasapiOutputDevice info;
                info.idUtf8 =
                    wideToUtf8(endpointId);
                info.nameUtf8 =
                    friendlyName(device);

                if (info.nameUtf8.empty())
                {
                    info.nameUtf8 =
                        info.idUtf8;
                }

                result.push_back(
                    std::move(info));

                CoTaskMemFree(
                    endpointId);
            }

            device->Release();
        }
    }

    if (collection != nullptr)
    {
        collection->Release();
    }

    if (enumerator != nullptr)
    {
        enumerator->Release();
    }

    if (uninitializeCom)
    {
        CoUninitialize();
    }

    std::sort(
        result.begin(),
        result.end(),
        [](const auto& left, const auto& right)
        {
            return
                left.nameUtf8 <
                right.nameUtf8;
        });
#endif

    return result;
}

WasapiPcmOutput::WasapiPcmOutput()
{
    thread_ =
        std::thread(
            [this]()
            {
                workerLoop();
            });
}

WasapiPcmOutput::~WasapiPcmOutput()
{
    stop_.store(
        true,
        std::memory_order_release);

    deviceGeneration_.fetch_add(
        1,
        std::memory_order_acq_rel);

    if (thread_.joinable())
    {
        thread_.join();
    }
}

void WasapiPcmOutput::setEnabled(
    bool enabled)
{
    enabled_.store(
        enabled,
        std::memory_order_release);

    if (!enabled)
    {
        clear();
    }
}

void WasapiPcmOutput::setDeviceId(
    const std::string& endpointIdUtf8)
{
    bool changed = false;

    {
        std::lock_guard<std::mutex>
            lock(deviceMutex_);

        if (deviceIdUtf8_ !=
            endpointIdUtf8)
        {
            deviceIdUtf8_ =
                endpointIdUtf8;
            changed = true;
        }
    }

    if (changed)
    {
        // Make the current render loop exit. The worker then reopens the
        // newly selected endpoint without requiring PCM output to be toggled.
        deviceGeneration_.fetch_add(
            1,
            std::memory_order_acq_rel);

        clear();
    }
}

std::string
WasapiPcmOutput::deviceId() const
{
    std::lock_guard<std::mutex>
        lock(deviceMutex_);

    return deviceIdUtf8_;
}

void WasapiPcmOutput::setBufferDepthMs(
    int milliseconds)
{
    constexpr int allowed[] = {0, 4, 8, 16, 32, 64, 128};
    int best = allowed[0];
    int bestDistance = std::abs(milliseconds - best);
    for (const int value : allowed)
    {
        const int distance = std::abs(milliseconds - value);
        if (distance < bestDistance)
        {
            best = value;
            bestDistance = distance;
        }
    }

    const int previous = bufferDepthMs_.exchange(best, std::memory_order_acq_rel);
    if (previous != best)
    {
        clear();
        deviceGeneration_.fetch_add(1, std::memory_order_acq_rel);
    }
}

void WasapiPcmOutput::clear()
{
    std::lock_guard<std::mutex>
        lock(mutex_);

    fifo_.clear();
}

void WasapiPcmOutput::pushStereo44100(
    const std::vector<std::int16_t>&
        interleavedStereo)
{
    if (!enabled_.load(
            std::memory_order_acquire) ||
        interleavedStereo.empty())
    {
        return;
    }

    std::lock_guard<std::mutex>
        lock(mutex_);

    const std::size_t maxSamples =
        kMaxQueuedFrames *
        kChannels;

    for (const auto sample :
         interleavedStereo)
    {
        fifo_.push_back(
            sample);
    }

    while (fifo_.size() >
           maxSamples)
    {
        fifo_.pop_front();

        if (!fifo_.empty())
        {
            fifo_.pop_front();
        }
    }
}

void WasapiPcmOutput::workerLoop()
{
#ifdef _WIN32
    const HRESULT coHr =
        CoInitializeEx(
            nullptr,
            COINIT_MULTITHREADED);

    const bool uninitializeCom =
        SUCCEEDED(coHr);

    while (!stop_.load(
        std::memory_order_acquire))
    {
        if (!enabled_.load(
            std::memory_order_acquire))
        {
            std::this_thread::sleep_for(
                std::chrono::milliseconds(20));
            continue;
        }

        const std::uint64_t
            openGeneration =
                deviceGeneration_.load(
                    std::memory_order_acquire);

        const std::string
            selectedId =
                deviceId();

        IMMDeviceEnumerator*
            enumerator = nullptr;
        IMMDevice*
            device = nullptr;
        IAudioClient*
            client = nullptr;
        IAudioRenderClient*
            render = nullptr;
        HANDLE
            eventHandle = nullptr;

        HRESULT hr =
            CoCreateInstance(
                __uuidof(
                    MMDeviceEnumerator),
                nullptr,
                CLSCTX_ALL,
                __uuidof(
                    IMMDeviceEnumerator),
                reinterpret_cast<void**>(
                    &enumerator));

        if (SUCCEEDED(hr))
        {
            if (selectedId.empty())
            {
                hr =
                    enumerator
                    ->GetDefaultAudioEndpoint(
                        eRender,
                        eConsole,
                        &device);
            }
            else
            {
                const auto wideId =
                    utf8ToWide(
                        selectedId);

                hr =
                    enumerator->GetDevice(
                        wideId.c_str(),
                        &device);
            }
        }

        if (SUCCEEDED(hr))
        {
            hr =
                device->Activate(
                    __uuidof(
                        IAudioClient),
                    CLSCTX_ALL,
                    nullptr,
                    reinterpret_cast<void**>(
                        &client));
        }

        WAVEFORMATEX format{};
        format.wFormatTag =
            WAVE_FORMAT_PCM;
        format.nChannels = 2;
        format.nSamplesPerSec =
            44100;
        format.wBitsPerSample = 16;
        format.nBlockAlign = 4;
        format.nAvgBytesPerSec =
            format.nSamplesPerSec *
            format.nBlockAlign;

        eventHandle =
            CreateEventW(
                nullptr,
                FALSE,
                FALSE,
                nullptr);

        if (SUCCEEDED(hr) &&
            eventHandle != nullptr)
        {
            constexpr DWORD
                streamFlags =
                    AUDCLNT_STREAMFLAGS_EVENTCALLBACK |
                    AUDCLNT_STREAMFLAGS_NOPERSIST |
                    AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM |
                    AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;

            hr =
                client->Initialize(
                    AUDCLNT_SHAREMODE_SHARED,
                    streamFlags,
                    0,
                    0,
                    &format,
                    nullptr);
        }

        if (SUCCEEDED(hr))
        {
            hr =
                client->SetEventHandle(
                    eventHandle);
        }

        if (SUCCEEDED(hr))
        {
            hr =
                client->GetService(
                    __uuidof(
                        IAudioRenderClient),
                    reinterpret_cast<void**>(
                        &render));
        }

        UINT32 bufferFrames = 0;

        if (SUCCEEDED(hr))
        {
            hr =
                client->GetBufferSize(
                    &bufferFrames);
        }

        if (SUCCEEDED(hr))
        {
            BYTE* data = nullptr;

            if (SUCCEEDED(
                    render->GetBuffer(
                        bufferFrames,
                        &data)))
            {
                render->ReleaseBuffer(
                    bufferFrames,
                    AUDCLNT_BUFFERFLAGS_SILENT);
            }

            hr =
                client->Start();
        }

        bool playoutPrimed = bufferDepthMs_.load(std::memory_order_acquire) == 0;

        while (
            SUCCEEDED(hr) &&
            enabled_.load(
                std::memory_order_acquire) &&
            !stop_.load(
                std::memory_order_acquire) &&
            deviceGeneration_.load(
                std::memory_order_acquire) ==
                openGeneration)
        {
            const DWORD wait =
                WaitForSingleObject(
                    eventHandle,
                    100);

            if (wait !=
                WAIT_OBJECT_0)
            {
                continue;
            }

            UINT32 padding = 0;

            if (FAILED(
                    client
                    ->GetCurrentPadding(
                        &padding)))
            {
                break;
            }

            const UINT32 frames =
                bufferFrames > padding
                    ? bufferFrames -
                        padding
                    : 0;

            if (frames == 0)
            {
                continue;
            }

            BYTE* bytes = nullptr;

            if (FAILED(
                    render->GetBuffer(
                        frames,
                        &bytes)))
            {
                break;
            }

            auto* out =
                reinterpret_cast<
                    std::int16_t*>(
                        bytes);

            bool anyAudio = false;

            {
                std::lock_guard<
                    std::mutex>
                    lock(mutex_);

                const int depthMs = bufferDepthMs_.load(std::memory_order_acquire);
                const std::size_t targetFrames =
                    depthMs <= 0
                        ? 0u
                        : static_cast<std::size_t>((44100LL * depthMs + 999) / 1000);

                const std::size_t queuedFrames = fifo_.size() / kChannels;
                if (!playoutPrimed && queuedFrames >= targetFrames)
                {
                    playoutPrimed = true;
                }

                bool underrun = false;
                for (UINT32 f = 0;
                     f < frames;
                     ++f)
                {
                    if (playoutPrimed && fifo_.size() >= kChannels)
                    {
                        for (std::size_t ch = 0; ch < kChannels; ++ch)
                        {
                            *out++ = fifo_.front();
                            fifo_.pop_front();
                        }
                        anyAudio = true;
                    }
                    else
                    {
                        *out++ = 0;
                        *out++ = 0;
                        if (playoutPrimed)
                        {
                            underrun = true;
                        }
                    }
                }

                if (underrun && targetFrames > 0)
                {
                    // Re-prime after a real receiver underrun. This trades a
                    // short mute for avoiding repeated crackles while Windows
                    // or another thread briefly stalls the PCM producer.
                    playoutPrimed = false;
                }
            }

            render->ReleaseBuffer(
                frames,
                anyAudio
                    ? 0
                    : AUDCLNT_BUFFERFLAGS_SILENT);
        }

        if (client != nullptr)
        {
            client->Stop();
        }

        if (render != nullptr)
        {
            render->Release();
        }

        if (client != nullptr)
        {
            client->Release();
        }

        if (device != nullptr)
        {
            device->Release();
        }

        if (enumerator != nullptr)
        {
            enumerator->Release();
        }

        if (eventHandle != nullptr)
        {
            CloseHandle(
                eventHandle);
        }

        if (enabled_.load(
                std::memory_order_acquire) &&
            !stop_.load(
                std::memory_order_acquire) &&
            deviceGeneration_.load(
                std::memory_order_acquire) ==
                openGeneration)
        {
            // An endpoint disappeared or failed to initialize. Avoid a tight
            // reopen loop; selecting another device wakes the next iteration
            // through generation change.
            std::this_thread::sleep_for(
                std::chrono::milliseconds(250));
        }
    }

    if (uninitializeCom)
    {
        CoUninitialize();
    }
#else
    while (!stop_.load(
        std::memory_order_acquire))
    {
        std::this_thread::sleep_for(
            std::chrono::milliseconds(100));
    }
#endif
}
