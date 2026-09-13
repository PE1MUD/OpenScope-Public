#include "DeckLinkProbe.h"
#include "VideoEngine.h"
#include <DeckLinkAPI_h.h>
#include "DeckLinkInputCallback.h"
#include "Video/Uyvy422ToYuv444Converter.h"
#include "Video/V210ToYuv444Converter.h"

#include <algorithm>
#include <QDebug>
#include <QMessageBox>
#include <QString>
#include <QStringList>

static Uyvy422ToYuv444Converter uyvyConverter;
static V210ToYuv444Converter v210Converter;

static QString shortDeckLinkName(const QString& apiName)
{
    QString name = apiName.simplified();

    if (name.contains(
            QStringLiteral("Intensity Pro 4K"),
            Qt::CaseInsensitive))
    {
        return QStringLiteral("BMD IP 4K");
    }

    if (name.contains(
            QStringLiteral("Intensity Pro"),
            Qt::CaseInsensitive))
    {
        return QStringLiteral("BMD IP");
    }

    name.replace(
        QStringLiteral("Blackmagic Design"),
        QString(),
        Qt::CaseInsensitive);

    name.replace(
        QStringLiteral("Blackmagic"),
        QString(),
        Qt::CaseInsensitive);

    name.replace(
        QStringLiteral("DeckLink"),
        QString(),
        Qt::CaseInsensitive);

    name = name.simplified();

    if (name.isEmpty())
    {
        return QStringLiteral("BMD");
    }

    return QStringLiteral("BMD %1").arg(name);
}

static QString deckLinkApiName(IDeckLink* deckLink)
{
    BSTR name = nullptr;

    if (deckLink->GetModelName(&name) == S_OK &&
        name != nullptr)
    {
        const QString result =
            QString::fromWCharArray(name);

        SysFreeString(name);
        return result;
    }

    name = nullptr;

    if (deckLink->GetDisplayName(&name) == S_OK &&
        name != nullptr)
    {
        const QString result =
            QString::fromWCharArray(name);

        SysFreeString(name);
        return result;
    }

    return {};
}

static void dumpConnections(const char* label, int64_t value)
{
    qDebug() << label;

    if (value & bmdVideoConnectionSDI)
        qDebug() << "    SDI";

    if (value & bmdVideoConnectionHDMI)
        qDebug() << "    HDMI";

    if (value & bmdVideoConnectionOpticalSDI)
        qDebug() << "    Optical SDI";

    if (value & bmdVideoConnectionComponent)
        qDebug() << "    Component";

    if (value & bmdVideoConnectionComposite)
        qDebug() << "    Composite";

    if (value & bmdVideoConnectionSVideo)
        qDebug() << "    S-Video";
}

static void dumpDisplayModes(IDeckLink* deckLink)
{
    IDeckLinkInput* input = nullptr;

    if (deckLink->QueryInterface(
        IID_IDeckLinkInput,
        reinterpret_cast<void**>(&input)) != S_OK)
    {
        qDebug() << "  No capture interface";
        return;
    }

    IDeckLinkDisplayModeIterator* modeIterator = nullptr;

    if (input->GetDisplayModeIterator(&modeIterator) != S_OK)
    {
        qDebug() << "  Cannot enumerate display modes";
        input->Release();
        return;
    }

    qDebug() << "  Input modes:";

    IDeckLinkDisplayMode* mode = nullptr;

    while (modeIterator->Next(&mode) == S_OK)
    {
        BSTR name = nullptr;

        if (mode->GetName(&name) == S_OK)
        {
            BMDTimeValue frameDuration = 0;
            BMDTimeScale timeScale = 0;

            mode->GetFrameRate(&frameDuration, &timeScale);

            const double fps =
                frameDuration != 0
                ? static_cast<double>(timeScale) /
                static_cast<double>(frameDuration)
                : 0.0;

            qDebug().noquote()
                << QString("    %1 - %2x%3 - %4 fps")
                .arg(QString::fromWCharArray(name))
                .arg(mode->GetWidth())
                .arg(mode->GetHeight())
                .arg(fps, 0, 'f', 3);

            SysFreeString(name);
        }

        mode->Release();
    }

    modeIterator->Release();
    input->Release();
}

static IDeckLinkInput* activeInput = nullptr;
static DeckLinkInputCallback* activeCallback = nullptr;
static IDeckLinkConfiguration* activeConfiguration = nullptr;
static DeckLinkCompositeGainState activeCompositeGainState;
static bool activeConfigurationDirty = false;
static int preferredDeviceIndex = 0;
static int activeDeviceIndex = -1;
static QStringList availableDeviceNames;
static QList<bool> availableDeviceBusyStates;

static bool deckLinkCaptureBusy(
    IDeckLink* deckLink,
    int deviceIndex)
{
    if (deckLink == nullptr)
    {
        return true;
    }

    int64_t busyState = 0;
    bool haveBusyState = false;

    IDeckLinkStatus* status = nullptr;

    if (deckLink->QueryInterface(
            IID_IDeckLinkStatus,
            reinterpret_cast<void**>(&status)) == S_OK &&
        status != nullptr)
    {
        if (status->GetInt(
                bmdDeckLinkStatusBusy,
                &busyState) == S_OK)
        {
            haveBusyState = true;
        }

        status->Release();
    }

    // A capture-busy flag means another capture client owns the input.
    // The only exception is our own currently active OpenScope capture.
    if (haveBusyState &&
        (busyState & bmdDeviceCaptureBusy) != 0 &&
        !(deviceIndex == activeDeviceIndex &&
          activeInput != nullptr))
    {
        return true;
    }

    // Playback only blocks capture on half-duplex/simplex hardware.  A
    // full-duplex DeckLink may legitimately RX and TX at the same time.
    if (haveBusyState &&
        (busyState & bmdDevicePlaybackBusy) != 0)
    {
        int64_t duplexValue = 0;
        bool haveDuplex = false;

        IDeckLinkProfileAttributes* attributes = nullptr;

        if (deckLink->QueryInterface(
                IID_IDeckLinkProfileAttributes,
                reinterpret_cast<void**>(&attributes)) == S_OK &&
            attributes != nullptr)
        {
            if (attributes->GetInt(
                    BMDDeckLinkDuplex,
                    &duplexValue) == S_OK)
            {
                haveDuplex = true;
            }

            attributes->Release();
        }

        if (!haveDuplex ||
            static_cast<BMDDuplexMode>(duplexValue) != bmdDuplexFull)
        {
            return true;
        }
    }

    return false;
}

static void refreshAvailableDeviceList()
{
    availableDeviceNames.clear();
    availableDeviceBusyStates.clear();

    IDeckLinkIterator* iterator = nullptr;

    if (CoCreateInstance(
            CLSID_CDeckLinkIterator,
            nullptr,
            CLSCTX_ALL,
            IID_IDeckLinkIterator,
            reinterpret_cast<void**>(&iterator)) != S_OK ||
        iterator == nullptr)
    {
        return;
    }

    IDeckLink* deckLink = nullptr;
    int index = 0;

    while (iterator->Next(&deckLink) == S_OK)
    {
        const QString apiName =
            deckLinkApiName(deckLink);

        availableDeviceNames.append(
            apiName.isEmpty()
            ? QStringLiteral("Blackmagic device %1").arg(index + 1)
            : apiName);

        availableDeviceBusyStates.append(
            deckLinkCaptureBusy(
                deckLink,
                index));

        deckLink->Release();
        deckLink = nullptr;
        ++index;
    }

    iterator->Release();
}

static bool testPalInput(
    IDeckLink* deckLink,
    VideoEngine* videoEngine)
{
    IDeckLinkInput* input = nullptr;

    if (deckLink->QueryInterface(
        IID_IDeckLinkInput,
        reinterpret_cast<void**>(&input)) != S_OK)
    {
        qDebug() << "  No capture interface";
        return false;
    }

    if (activeConfiguration != nullptr)
    {
        if (activeConfigurationDirty)
        {
            activeConfiguration->WriteConfigurationToPreferences();
        }

        activeConfiguration->Release();
        activeConfiguration = nullptr;
    }

    activeCompositeGainState = {};
    activeConfigurationDirty = false;

    IDeckLinkConfiguration* configuration = nullptr;

    if (deckLink->QueryInterface(
            IID_IDeckLinkConfiguration,
            reinterpret_cast<void**>(&configuration)) == S_OK)
    {
        activeConfiguration = configuration;

        IDeckLinkProfileAttributes* attributes = nullptr;

        if (deckLink->QueryInterface(
                IID_IDeckLinkProfileAttributes,
                reinterpret_cast<void**>(&attributes)) == S_OK)
        {
            double minimumDb = 0.0;
            double maximumDb = 0.0;

            if (attributes->GetFloat(
                    BMDDeckLinkVideoInputGainMinimum,
                    &minimumDb) == S_OK &&
                attributes->GetFloat(
                    BMDDeckLinkVideoInputGainMaximum,
                    &maximumDb) == S_OK)
            {
                activeCompositeGainState.minimumDb = minimumDb;
                activeCompositeGainState.maximumDb = maximumDb;
            }

            attributes->Release();
        }

        double value = 0.0;

        if (activeConfiguration->GetFloat(
                bmdDeckLinkConfigVideoInputCompositeLumaGain,
                &value) == S_OK)
        {
            activeCompositeGainState.lumaAvailable = true;
            activeCompositeGainState.lumaDb = value;
        }

        value = 0.0;

        if (activeConfiguration->GetFloat(
                bmdDeckLinkConfigVideoInputCompositeChromaGain,
                &value) == S_OK)
        {
            activeCompositeGainState.chromaAvailable = true;
            activeCompositeGainState.chromaDb = value;
        }
    }

    activeInput = input;
    activeCallback = new DeckLinkInputCallback(
        videoEngine,
        &v210Converter);

    HRESULT result = input->SetCallback(activeCallback);

    if (FAILED(result))
    {
        qDebug() << "  Failed to set input callback";

        activeCallback->Release();
        activeCallback = nullptr;

        input->Release();
        activeInput = nullptr;
        return false;
    }

    result = input->EnableVideoInput(
        bmdModePAL,
        bmdFormat10BitYUV,
        bmdVideoInputFlagDefault);

    if (FAILED(result))
    {
        qDebug() << "  Failed to enable PAL input";

        input->SetCallback(nullptr);

        activeCallback->Release();
        activeCallback = nullptr;

        input->Release();
        activeInput = nullptr;
        return false;
    }

    result = input->StartStreams();

    if (SUCCEEDED(result))
    {
        qDebug() << "  PAL capture started";
        return true;
    }

    qDebug() << "  Failed to start PAL capture";

    input->DisableVideoInput();
    input->SetCallback(nullptr);

    activeCallback->Release();
    activeCallback = nullptr;

    input->Release();
    activeInput = nullptr;
    return false;
}

static bool dumpDevice(
    IDeckLink* deckLink,
    int index,
    VideoEngine* videoEngine,
    bool startCapture)
{
    const QString apiName =
        deckLinkApiName(deckLink);

    qDebug() << index << ":" << apiName;

    IDeckLinkProfileAttributes* attributes = nullptr;

    if (deckLink->QueryInterface(
        IID_IDeckLinkProfileAttributes,
        reinterpret_cast<void**>(&attributes)) == S_OK)
    {
        int64_t value = 0;

        if (attributes->GetInt(
            BMDDeckLinkVideoInputConnections,
            &value) == S_OK)
        {
            dumpConnections("  Video inputs:", value);
        }

        if (attributes->GetInt(
            BMDDeckLinkVideoOutputConnections,
            &value) == S_OK)
        {
            dumpConnections("  Video outputs:", value);
        }

        attributes->Release();
    }

    dumpDisplayModes(deckLink);

    if (startCapture)
    {
        qDebug() << "  Selected for capture";
        return testPalInput(
            deckLink,
            videoEngine);
    }

    return false;
}

QString deckLinkProbe(
    VideoEngine* videoEngine,
    int deviceIndex)
{
    // Probing may be requested from several UI paths. Always tear down a
    // previous capture here as well, so callers cannot accidentally leave an
    // older DeckLink callback feeding the same VideoEngine.
    deckLinkStop();

    if (deviceIndex < 0)
    {
        deviceIndex = preferredDeviceIndex;
    }

    deviceIndex = (std::max)(0, deviceIndex);

    // Build the complete device/status list before deciding which card to open.
    // A persisted half-duplex card may currently be occupied by a TX process.
    refreshAvailableDeviceList();

    if (availableDeviceNames.isEmpty())
    {
        IDeckLinkIterator* testIterator = nullptr;

        const HRESULT result = CoCreateInstance(
            CLSID_CDeckLinkIterator,
            nullptr,
            CLSCTX_ALL,
            IID_IDeckLinkIterator,
            reinterpret_cast<void**>(&testIterator));

        if (FAILED(result))
        {
            qDebug() << "No DeckLink driver found. HRESULT:"
                << QString::number(
                    static_cast<unsigned long>(result),
                    16);

            QMessageBox::warning(
                nullptr,
                "Blackmagic Desktop Video not found",
                "OpenScope requires Blackmagic Desktop Video 16.2 or later "
                "for DeckLink video capture.\n\n"
                "Please install Desktop Video 16.2 or later.\n\n"
                "OpenScope will continue without video capture.");

            return {};
        }

        if (testIterator != nullptr)
        {
            testIterator->Release();
        }

        qDebug() << "No DeckLink devices found.";

        QMessageBox::warning(
            nullptr,
            "No Blackmagic DeckLink device found",
            "Blackmagic Desktop Video is installed, but no compatible "
            "DeckLink capture device was detected.\n\n"
            "OpenScope will continue without video capture.");

        return {};
    }

    int selectedIndex = -1;

    if (deviceIndex >= 0 &&
        deviceIndex < availableDeviceBusyStates.size() &&
        !availableDeviceBusyStates.at(deviceIndex))
    {
        selectedIndex = deviceIndex;
    }
    else
    {
        for (int index = 0;
             index < availableDeviceBusyStates.size();
             ++index)
        {
            if (!availableDeviceBusyStates.at(index))
            {
                selectedIndex = index;
                break;
            }
        }
    }

    // All capture-capable cards are currently occupied. Keep the inventory
    // available so the Source menu can show [BUSY], but do not attempt RX.
    if (selectedIndex < 0)
    {
        qDebug() << "All DeckLink capture devices are busy.";
        return {};
    }

    IDeckLinkIterator* iterator = nullptr;

    const HRESULT result = CoCreateInstance(
        CLSID_CDeckLinkIterator,
        nullptr,
        CLSCTX_ALL,
        IID_IDeckLinkIterator,
        reinterpret_cast<void**>(&iterator));

    if (FAILED(result) ||
        iterator == nullptr)
    {
        return {};
    }

    IDeckLink* deckLink = nullptr;
    int index = 0;
    QString activeDeviceName;

    while (iterator->Next(&deckLink) == S_OK)
    {
        const QString apiName =
            deckLinkApiName(deckLink);

        const bool selected =
            index == selectedIndex;

        const bool captureStarted =
            dumpDevice(
                deckLink,
                index,
                videoEngine,
                selected);

        if (selected && captureStarted)
        {
            activeDeviceIndex = index;
            activeDeviceName =
                shortDeckLinkName(apiName);
        }

        deckLink->Release();
        deckLink = nullptr;
        ++index;
    }

    iterator->Release();

    // Refresh once more so our own newly active capture is not presented as
    // externally busy in the Source menu.
    refreshAvailableDeviceList();

    return activeDeviceName;
}


QStringList deckLinkDeviceNames()
{
    return availableDeviceNames;
}

QList<bool> deckLinkDeviceBusyStates()
{
    return availableDeviceBusyStates;
}

void deckLinkRefreshDeviceList()
{
    refreshAvailableDeviceList();
}

int deckLinkActiveDeviceIndex()
{
    return activeDeviceIndex;
}

void deckLinkSetPreferredDeviceIndex(
    int deviceIndex)
{
    preferredDeviceIndex =
        (std::max)(0, deviceIndex);
}

void deckLinkStop()
{
    if (activeInput != nullptr)
    {
        activeInput->SetCallback(nullptr);
        activeInput->StopStreams();
        activeInput->DisableVideoInput();

        activeInput->Release();
        activeInput = nullptr;
    }

    if (activeCallback != nullptr)
    {
        activeCallback->Release();
        activeCallback = nullptr;
    }

    if (activeConfiguration != nullptr)
    {
        if (activeConfigurationDirty)
        {
            activeConfiguration->WriteConfigurationToPreferences();
        }

        activeConfiguration->Release();
        activeConfiguration = nullptr;
    }

    activeCompositeGainState = {};
    activeConfigurationDirty = false;
    activeDeviceIndex = -1;

    qDebug() << "DeckLink capture stopped";
}

DeckLinkCompositeGainState deckLinkCompositeGainState()
{
    return activeCompositeGainState;
}

bool deckLinkSetCompositeLumaGain(double gainDb)
{
    if (activeConfiguration == nullptr ||
        !activeCompositeGainState.lumaAvailable)
    {
        return false;
    }

    gainDb = std::clamp(
        gainDb,
        activeCompositeGainState.minimumDb,
        activeCompositeGainState.maximumDb);

    if (activeConfiguration->SetFloat(
            bmdDeckLinkConfigVideoInputCompositeLumaGain,
            gainDb) != S_OK)
    {
        return false;
    }

    activeCompositeGainState.lumaDb = gainDb;
    activeConfigurationDirty = true;
    return true;
}

bool deckLinkSetCompositeChromaGain(double gainDb)
{
    if (activeConfiguration == nullptr ||
        !activeCompositeGainState.chromaAvailable)
    {
        return false;
    }

    gainDb = std::clamp(
        gainDb,
        activeCompositeGainState.minimumDb,
        activeCompositeGainState.maximumDb);

    if (activeConfiguration->SetFloat(
            bmdDeckLinkConfigVideoInputCompositeChromaGain,
            gainDb) != S_OK)
    {
        return false;
    }

    activeCompositeGainState.chromaDb = gainDb;
    activeConfigurationDirty = true;
    return true;
}

bool deckLinkCommitConfiguration()
{
    if (activeConfiguration == nullptr)
    {
        return false;
    }

    if (!activeConfigurationDirty)
    {
        return true;
    }

    if (activeConfiguration->WriteConfigurationToPreferences() != S_OK)
    {
        return false;
    }

    activeConfigurationDirty = false;
    return true;
}
