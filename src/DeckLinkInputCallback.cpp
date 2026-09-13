#include "DeckLinkInputCallback.h"
#include "VideoEngine.h"

#include <QDebug>
#include <QElapsedTimer>

DeckLinkInputCallback::DeckLinkInputCallback(
    VideoEngine* videoEngine,
    const VideoConverter* converter)
    : videoEngine_(videoEngine)
    , converter_(converter)
{}

HRESULT STDMETHODCALLTYPE DeckLinkInputCallback::QueryInterface(
    REFIID iid,
    LPVOID* ppv)
{
    if (ppv == nullptr)
        return E_POINTER;

    *ppv = nullptr;

    if (iid == IID_IUnknown ||
        iid == IID_IDeckLinkInputCallback)
    {
        *ppv = static_cast<IDeckLinkInputCallback*>(this);
        AddRef();
        return S_OK;
    }

    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE DeckLinkInputCallback::AddRef()
{
    return ++refCount_;
}

ULONG STDMETHODCALLTYPE DeckLinkInputCallback::Release()
{
    const ULONG count = --refCount_;

    if (count == 0)
        delete this;

    return count;
}

HRESULT STDMETHODCALLTYPE DeckLinkInputCallback::VideoInputFormatChanged(
    BMDVideoInputFormatChangedEvents,
    IDeckLinkDisplayMode*,
    BMDDetectedVideoInputFormatFlags)
{
    qDebug() << "DeckLink input format changed";
    return S_OK;
}

HRESULT STDMETHODCALLTYPE DeckLinkInputCallback::VideoInputFrameArrived(
    IDeckLinkVideoInputFrame* videoFrame,
    IDeckLinkAudioInputPacket*)
{
    static QElapsedTimer captureFpsTimer;
    static int captureFrameCount = 0;

    if (!captureFpsTimer.isValid())
    {
        captureFpsTimer.start();
    }

    ++captureFrameCount;

    //if (captureFpsTimer.elapsed() >= 1000)
    //{
    //    qDebug()
    //        << "DeckLink callback FPS ="
    //        << captureFrameCount;

        captureFrameCount = 0;
    //    captureFpsTimer.restart();
    //}
    if (videoFrame == nullptr || videoEngine_ == nullptr)
        return S_OK;

    IDeckLinkVideoBuffer* videoBuffer = nullptr;

    if (videoFrame->QueryInterface(
        IID_IDeckLinkVideoBuffer,
        reinterpret_cast<void**>(&videoBuffer)) != S_OK)
    {
        return S_OK;
    }

    if (videoBuffer->StartAccess(bmdBufferAccessRead) != S_OK)
    {
        videoBuffer->Release();
        return S_OK;
    }

    void* bytes = nullptr;

    if (videoBuffer->GetBytes(&bytes) != S_OK || bytes == nullptr)
    {
        videoBuffer->EndAccess(bmdBufferAccessRead);
        videoBuffer->Release();
        return S_OK;
    }

    const int width =
        static_cast<int>(videoFrame->GetWidth());

    const int height =
        static_cast<int>(videoFrame->GetHeight());

    const int rowBytes =
        static_cast<int>(videoFrame->GetRowBytes());

    const BMDFrameFlags frameFlags =
        videoFrame->GetFlags();

    const bool inputSignalValid =
        (frameFlags & bmdFrameHasNoInputSource) == 0;

    const auto* source =
        static_cast<const std::uint8_t*>(bytes);

    if (auto* frame = videoEngine_->tryAcquireWriteFrame())
    {
        const bool ok =
            converter_->convert(
                source,
                rowBytes,
                width,
                height,
                *frame);

        if (ok)
        {
            frame->inputSignalValid =
                inputSignalValid;

            videoEngine_->submitWriteFrame();
        }
        else
        {
            videoEngine_->cancelWriteFrame();
        }
    }

    videoBuffer->EndAccess(bmdBufferAccessRead);
    videoBuffer->Release();

    return S_OK;
}