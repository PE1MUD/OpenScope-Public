#include "WasapiCapture.h"
#include "Pcm16VideoEncoder.h"

#include <Windows.h>
#include <Audioclient.h>
#include <Mmdeviceapi.h>
#include <Functiondiscoverykeys_devpkey.h>
#include <Propsys.h>
#include <mmreg.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace
{
std::wstring friendlyName(IMMDevice* device)
{
    IPropertyStore* store = nullptr;
    PROPVARIANT value;
    PropVariantInit(&value);

    std::wstring result = L"Audio input";

    if (SUCCEEDED(device->OpenPropertyStore(STGM_READ, &store)) && store != nullptr)
    {
        if (SUCCEEDED(store->GetValue(PKEY_Device_FriendlyName, &value)) &&
            value.vt == VT_LPWSTR &&
            value.pwszVal != nullptr)
        {
            result = value.pwszVal;
        }

        PropVariantClear(&value);
        store->Release();
    }

    return result;
}

bool isFloatFormat(const WAVEFORMATEX* format)
{
    if (format == nullptr)
        return false;

    if (format->wFormatTag == WAVE_FORMAT_IEEE_FLOAT)
        return true;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_IEEE_FLOAT);
    }

    return false;
}

bool isPcmFormat(const WAVEFORMATEX* format)
{
    if (format == nullptr)
        return false;

    if (format->wFormatTag == WAVE_FORMAT_PCM)
        return true;

    if (format->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        format->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        const auto* ext = reinterpret_cast<const WAVEFORMATEXTENSIBLE*>(format);
        return IsEqualGUID(ext->SubFormat, KSDATAFORMAT_SUBTYPE_PCM);
    }

    return false;
}

float pcmSampleToFloat(
    const std::uint8_t* sample,
    int bitsPerSample,
    int validBits)
{
    if (sample == nullptr)
        return 0.0f;

    if (bitsPerSample == 16)
    {
        std::int16_t value = 0;
        std::memcpy(&value, sample, sizeof(value));
        return static_cast<float>(value) / 32768.0f;
    }

    if (bitsPerSample == 24)
    {
        std::int32_t value =
            static_cast<std::int32_t>(sample[0]) |
            (static_cast<std::int32_t>(sample[1]) << 8) |
            (static_cast<std::int32_t>(sample[2]) << 16);

        if ((value & 0x00800000) != 0)
            value |= static_cast<std::int32_t>(0xFF000000);

        return static_cast<float>(value) / 8388608.0f;
    }

    if (bitsPerSample == 32)
    {
        std::int32_t value = 0;
        std::memcpy(&value, sample, sizeof(value));

        const int effectiveBits =
            validBits > 0 && validBits <= 32 ? validBits : 32;

        if (effectiveBits < 32)
            value >>= (32 - effectiveBits);

        const double denom =
            static_cast<double>(std::uint64_t{1} << (effectiveBits - 1));

        return static_cast<float>(
            static_cast<double>(value) / denom);
    }

    return 0.0f;
}
}

WasapiCapture::WasapiCapture() = default;

WasapiCapture::~WasapiCapture()
{
    stop();
}

std::vector<AudioInputDevice> WasapiCapture::enumerate()
{
    std::vector<AudioInputDevice> result;

    const HRESULT coHr =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(coHr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDeviceCollection* collection = nullptr;

    if (SUCCEEDED(
            CoCreateInstance(
                __uuidof(MMDeviceEnumerator),
                nullptr,
                CLSCTX_ALL,
                __uuidof(IMMDeviceEnumerator),
                reinterpret_cast<void**>(&enumerator))) &&
        SUCCEEDED(
            enumerator->EnumAudioEndpoints(
                eCapture,
                DEVICE_STATE_ACTIVE,
                &collection)))
    {
        UINT count = 0;
        collection->GetCount(&count);

        for (UINT i = 0; i < count; ++i)
        {
            IMMDevice* device = nullptr;
            LPWSTR id = nullptr;

            if (SUCCEEDED(collection->Item(i, &device)) &&
                SUCCEEDED(device->GetId(&id)))
            {
                result.push_back({id, friendlyName(device)});
                CoTaskMemFree(id);
            }

            if (device != nullptr)
                device->Release();
        }
    }

    if (collection != nullptr)
        collection->Release();

    if (enumerator != nullptr)
        enumerator->Release();

    // Important: do not unbalance COM initialization performed by Qt on the
    // GUI thread when CoInitializeEx returned RPC_E_CHANGED_MODE.
    if (uninitializeCom)
        CoUninitialize();

    return result;
}

bool WasapiCapture::start(const std::wstring& id)
{
    stop();

    {
        std::lock_guard lock(mutex_);
        error_.clear();
        asrc_.reset(48000.0);
    }

    inputRate_ = 0.0;
    asrcPpm_ = 0.0;
    peakL_ = 0.0f;
    peakR_ = 0.0f;

    run_ = true;
    thread_ = std::thread(&WasapiCapture::worker, this, id);
    return true;
}

void WasapiCapture::stop()
{
    run_ = false;

    if (thread_.joinable())
        thread_.join();
}

std::size_t WasapiCapture::queuedFrames() const
{
    std::lock_guard lock(mutex_);
    return asrc_.queuedInputFrames();
}

bool WasapiCapture::readyForPalFrame() const
{
    if (inputRate_.load() <= 1.0)
        return false;

    // Start only when the elastic FIFO has reached its actual servo target.
    // Delta 0.2.1 only waited for roughly one PAL block (~2016 input frames),
    // which left essentially no reserve after each 1764-frame output pull.
    return queuedFrames() >= AudioAsrc::targetInputFrames();
}

std::uint64_t WasapiCapture::underruns() const
{
    std::lock_guard lock(mutex_);
    return asrc_.underruns();
}

std::wstring WasapiCapture::lastError() const
{
    std::lock_guard lock(mutex_);
    return error_;
}

std::vector<std::int16_t> WasapiCapture::takeStereoFrames(std::size_t frames)
{
    std::lock_guard lock(mutex_);
    auto result = asrc_.takeInt16(frames);
    asrcPpm_ = asrc_.correctionPpm();
    return result;
}

void WasapiCapture::worker(std::wstring id)
{
    const HRESULT coHr =
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    const bool uninitializeCom = SUCCEEDED(coHr);

    IMMDeviceEnumerator* enumerator = nullptr;
    IMMDevice* device = nullptr;
    IAudioClient* audioClient = nullptr;
    IAudioCaptureClient* captureClient = nullptr;
    WAVEFORMATEX* mixFormat = nullptr;

    HANDLE eventHandle =
        CreateEventW(nullptr, FALSE, FALSE, nullptr);

    HRESULT hr =
        CoCreateInstance(
            __uuidof(MMDeviceEnumerator),
            nullptr,
            CLSCTX_ALL,
            __uuidof(IMMDeviceEnumerator),
            reinterpret_cast<void**>(&enumerator));

    if (SUCCEEDED(hr))
        hr = enumerator->GetDevice(id.c_str(), &device);

    if (SUCCEEDED(hr))
        hr = device->Activate(
            __uuidof(IAudioClient),
            CLSCTX_ALL,
            nullptr,
            reinterpret_cast<void**>(&audioClient));

    if (SUCCEEDED(hr))
        hr = audioClient->GetMixFormat(&mixFormat);

    if (SUCCEEDED(hr) && mixFormat != nullptr)
    {
        inputRate_ = static_cast<double>(mixFormat->nSamplesPerSec);

        std::lock_guard lock(mutex_);
        asrc_.reset(static_cast<double>(mixFormat->nSamplesPerSec));
    }

    if (SUCCEEDED(hr))
    {
        hr = audioClient->Initialize(
            AUDCLNT_SHAREMODE_SHARED,
            AUDCLNT_STREAMFLAGS_EVENTCALLBACK,
            0,
            0,
            mixFormat,
            nullptr);
    }

    if (SUCCEEDED(hr))
        hr = audioClient->SetEventHandle(eventHandle);

    if (SUCCEEDED(hr))
        hr = audioClient->GetService(
            __uuidof(IAudioCaptureClient),
            reinterpret_cast<void**>(&captureClient));

    if (SUCCEEDED(hr))
        hr = audioClient->Start();

    if (FAILED(hr))
    {
        std::lock_guard lock(mutex_);
        error_ =
            L"WASAPI initialization failed: 0x" +
            std::to_wstring(static_cast<unsigned long>(hr));
    }

    const bool floatFormat = isFloatFormat(mixFormat);
    const bool pcmFormat = isPcmFormat(mixFormat);

    int validBits = mixFormat != nullptr
        ? mixFormat->wBitsPerSample
        : 0;

    if (mixFormat != nullptr &&
        mixFormat->wFormatTag == WAVE_FORMAT_EXTENSIBLE &&
        mixFormat->cbSize >= sizeof(WAVEFORMATEXTENSIBLE) - sizeof(WAVEFORMATEX))
    {
        validBits =
            reinterpret_cast<WAVEFORMATEXTENSIBLE*>(mixFormat)
                ->Samples.wValidBitsPerSample;
    }

    if (SUCCEEDED(hr) &&
        mixFormat != nullptr &&
        (!floatFormat && !pcmFormat))
    {
        std::lock_guard lock(mutex_);
        error_ = L"Unsupported WASAPI mix format.";
        hr = E_FAIL;
    }

    if (SUCCEEDED(hr) &&
        mixFormat != nullptr &&
        mixFormat->nChannels < 1)
    {
        std::lock_guard lock(mutex_);
        error_ = L"WASAPI source has no channels.";
        hr = E_FAIL;
    }

    std::vector<float> stereo;

    while (SUCCEEDED(hr) && run_)
    {
        if (WaitForSingleObject(eventHandle, 100) != WAIT_OBJECT_0)
            continue;

        UINT32 packetFrames = 0;
        captureClient->GetNextPacketSize(&packetFrames);

        while (packetFrames != 0 && run_)
        {
            BYTE* data = nullptr;
            UINT32 frames = 0;
            DWORD flags = 0;

            if (FAILED(
                    captureClient->GetBuffer(
                        &data,
                        &frames,
                        &flags,
                        nullptr,
                        nullptr)))
            {
                break;
            }

            stereo.assign(static_cast<std::size_t>(frames) * 2, 0.0f);

            float peakL = 0.0f;
            float peakR = 0.0f;

            if ((flags & AUDCLNT_BUFFERFLAGS_SILENT) == 0 &&
                data != nullptr &&
                mixFormat != nullptr)
            {
                const int channels = mixFormat->nChannels;
                const int bytesPerSample =
                    mixFormat->wBitsPerSample / 8;
                const int bytesPerFrame =
                    mixFormat->nBlockAlign;

                for (UINT32 i = 0; i < frames; ++i)
                {
                    const auto* frame =
                        data + static_cast<std::size_t>(i) * bytesPerFrame;

                    auto readChannel =
                        [&](int channel) -> float
                        {
                            const int selected =
                                std::min(channel, channels - 1);

                            const auto* sample =
                                frame +
                                static_cast<std::size_t>(selected) *
                                    bytesPerSample;

                            if (floatFormat &&
                                mixFormat->wBitsPerSample == 32)
                            {
                                float value = 0.0f;
                                std::memcpy(
                                    &value,
                                    sample,
                                    sizeof(value));

                                return std::clamp(
                                    value,
                                    -1.0f,
                                    1.0f);
                            }

                            return std::clamp(
                                pcmSampleToFloat(
                                    sample,
                                    mixFormat->wBitsPerSample,
                                    validBits),
                                -1.0f,
                                1.0f);
                        };

                    const float l = readChannel(0);
                    const float r =
                        channels >= 2 ? readChannel(1) : l;

                    stereo[static_cast<std::size_t>(i) * 2] = l;
                    stereo[static_cast<std::size_t>(i) * 2 + 1] = r;

                    peakL = std::max(peakL, std::abs(l));
                    peakR = std::max(peakR, std::abs(r));
                }
            }

            {
                std::lock_guard lock(mutex_);
                asrc_.pushInterleavedFloat(
                    stereo.data(),
                    frames);
            }

            peakL_ = peakL;
            peakR_ = peakR;

            captureClient->ReleaseBuffer(frames);
            captureClient->GetNextPacketSize(&packetFrames);
        }
    }

    if (audioClient != nullptr)
        audioClient->Stop();

    if (captureClient != nullptr)
        captureClient->Release();

    if (audioClient != nullptr)
        audioClient->Release();

    if (device != nullptr)
        device->Release();

    if (enumerator != nullptr)
        enumerator->Release();

    if (mixFormat != nullptr)
        CoTaskMemFree(mixFormat);

    if (eventHandle != nullptr)
        CloseHandle(eventHandle);

    if (uninitializeCom)
        CoUninitialize();
}
