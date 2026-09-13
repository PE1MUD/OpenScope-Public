#pragma once

#include <DeckLinkAPI_h.h>

#include "Video/VideoConverter.h"

class VideoEngine;

class DeckLinkInputCallback final : public IDeckLinkInputCallback
{
public:
    DeckLinkInputCallback(
        VideoEngine* videoEngine,
        const VideoConverter* converter);

    HRESULT STDMETHODCALLTYPE QueryInterface(
        REFIID iid,
        LPVOID* ppv) override;

    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    HRESULT STDMETHODCALLTYPE VideoInputFormatChanged(
        BMDVideoInputFormatChangedEvents notificationEvents,
        IDeckLinkDisplayMode* newDisplayMode,
        BMDDetectedVideoInputFormatFlags detectedSignalFlags) override;

    HRESULT STDMETHODCALLTYPE VideoInputFrameArrived(
        IDeckLinkVideoInputFrame* videoFrame,
        IDeckLinkAudioInputPacket* audioPacket) override;

private:
    ULONG refCount_ = 1;
    unsigned int frameCount_ = 0;

    VideoEngine* videoEngine_ = nullptr;
    const VideoConverter* converter_ = nullptr;
};