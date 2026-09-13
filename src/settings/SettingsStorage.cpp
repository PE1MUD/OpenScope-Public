#include "SettingsStorage.h"

#include <algorithm>
#include <QCoreApplication>
#include <QDir>
#include <QSettings>

namespace
{

    QString aspectRatioToString(
        OpenScopeSettings::AspectRatio aspectRatio)
    {
        switch (aspectRatio)
        {
        case OpenScopeSettings::AspectRatio::Ratio4x3:
            return "4:3";

        case OpenScopeSettings::AspectRatio::Ratio16x9:
            return "16:9";
        }

        return "4:3";
    }

    OpenScopeSettings::AspectRatio aspectRatioFromString(
        const QString& value,
        OpenScopeSettings::AspectRatio defaultValue)
    {
        if (value == "4:3")
        {
            return
                OpenScopeSettings::AspectRatio::Ratio4x3;
        }

        if (value == "16:9")
        {
            return
                OpenScopeSettings::AspectRatio::Ratio16x9;
        }

        return defaultValue;
    }
    QString workspaceViewToString(
        OpenScopeSettings::WorkspaceView view)
    {
        switch (view)
        {
        case OpenScopeSettings::WorkspaceView::Headless:
            return "Headless";

        case OpenScopeSettings::WorkspaceView::Matrix:
            return "Matrix";

        case OpenScopeSettings::WorkspaceView::Video:
            return "Video";

        case OpenScopeSettings::WorkspaceView::Waveform:
            return "Waveform";

        case OpenScopeSettings::WorkspaceView::Vectorscope:
            return "Vectorscope";
        }

        return "Matrix";
    }

    OpenScopeSettings::WorkspaceView workspaceViewFromString(
        const QString& value,
        OpenScopeSettings::WorkspaceView defaultValue)
    {
        if (value == "Headless")
            return OpenScopeSettings::WorkspaceView::Headless;

        if (value == "Matrix")
            return OpenScopeSettings::WorkspaceView::Matrix;

        if (value == "Video")
            return OpenScopeSettings::WorkspaceView::Video;

        if (value == "Waveform")
            return OpenScopeSettings::WorkspaceView::Waveform;

        if (value == "Vectorscope")
            return OpenScopeSettings::WorkspaceView::Vectorscope;

        return defaultValue;
    }
}

SettingsStorage::SettingsStorage()
{
    fileName_ =
        QDir(
            QCoreApplication::applicationDirPath())
        .filePath(
            "OpenScope.ini");
}

OpenScopeSettings SettingsStorage::load() const
{
    OpenScopeSettings result;

    QSettings settings(
        fileName_,
        QSettings::IniFormat);

    result.local
        .display
        .gamma =
        settings.value(
            "Local/Display/Gamma",
            result.local
            .display
            .gamma)
        .toDouble();

    result.local
        .display
        .aspectRatio =
        aspectRatioFromString(
            settings.value(
                "Local/Display/AspectRatio",
                aspectRatioToString(
                    result.local
                    .display
                    .aspectRatio))
            .toString(),
            result.local
            .display
            .aspectRatio);

    result.local
        .display
        .deinterlace =
        settings.value(
            "Local/Display/Deinterlace",
            result.local
            .display
            .deinterlace)
        .toBool();

    result.local.display.lineSelectorVisible =
        settings.value(
            "Local/Display/LineSelectorVisible",
            result.local.display.lineSelectorVisible)
        .toBool();

    result.local.display.safetyArea90 =
        settings.value(
            "Local/Display/SafetyArea90",
            result.local.display.safetyArea90)
        .toBool();

    result.local.display.textSafetyArea80 =
        settings.value(
            "Local/Display/TextSafetyArea80",
            result.local.display.textSafetyArea80)
        .toBool();

    result.local.display.preventDisplaySleep =
        settings.value(
            "Local/Display/PreventDisplaySleep",
            result.local.display.preventDisplaySleep)
        .toBool();

    result.local.display.followWssAspectRatio =
        settings.value(
            "Local/Display/FollowWssAspectRatio",
            result.local.display.followWssAspectRatio)
        .toBool();

    result.control
        .instrument
        .lineNumber =
        settings.value(
            "Control/Instrument/LineNumber",
            result.control
            .instrument
            .lineNumber)
        .toInt();

    result.control
        .instrument
        .waveform
        .antiAliasing =
        settings.value(
            "Control/Instrument/Waveform/AntiAliasing",
            settings.value(
                "Local/Display/AntiAliasing",
                result.control
                    .instrument
                    .waveform
                    .antiAliasing))
        .toBool();

    result.control
        .instrument
        .waveform
        .colorizeIllegalLuminance =
        settings.value(
            "Control/Instrument/Waveform/ColorizeIllegalLuminance",
            result.control
                .instrument
                .waveform
                .colorizeIllegalLuminance)
        .toBool();

    result.control
        .instrument
        .waveform
        .zoom =
        settings.value(
            "Control/Instrument/Waveform/Zoom",
            result.control
            .instrument
            .waveform
            .zoom)
        .toInt();

    result.control.instrument.waveform.scrollPosition =
        settings.value(
            "Control/Instrument/Waveform/ScrollPosition",
            result.control.instrument.waveform.scrollPosition)
        .toDouble();

    result.control
        .instrument
        .waveform
        .vintageLook =
        settings.value(
            "Control/Instrument/Waveform/VintageLook",
            result.control
            .instrument
            .waveform
            .vintageLook)
        .toBool();

    result.control
        .instrument
        .waveform
        .chromaRenderIntensity =
        settings.value(
            "Control/Instrument/Waveform/ChromaRenderIntensity",
            result.control
            .instrument
            .waveform
            .chromaRenderIntensity)
        .toInt();

    result.control
        .instrument
        .waveform
        .persistenceFrames =
        settings.value(
            "Control/Instrument/Waveform/PersistenceFrames",
            result.control
            .instrument
            .waveform
            .persistenceFrames)
        .toInt();

    result.control
        .instrument
        .waveform
        .coreIntensity =
        settings.value(
            "Control/Instrument/Waveform/CoreIntensity",
            result.control
            .instrument
            .waveform
            .coreIntensity)
        .toInt();

    result.control
        .instrument
        .waveform
        .coreWidthTenths =
        std::clamp(
            settings.value(
                "Control/Instrument/Waveform/CoreWidthTenths",
                result.control
                .instrument
                .waveform
                .coreWidthTenths)
            .toInt(),
            5,
            30);


    // Migration from the temporary 0..400 tuning scale:
    // old 200% is the new normal 100% beam level.
    if (result.control
        .instrument
        .waveform
        .coreIntensity > 100)
    {
        result.control
            .instrument
            .waveform
            .coreIntensity = 100;
    }

    if (result.control
        .instrument
        .waveform
        .coreIntensity < 0)
    {
        result.control
            .instrument
            .waveform
            .coreIntensity = 0;
    }

    result.control
        .instrument
        .vectorscope
        .showHundredPercentTargets =
        settings.value(
            "Control/Instrument/Vectorscope/ShowHundredPercentTargets",
            result.control
            .instrument
            .vectorscope
            .showHundredPercentTargets)
        .toBool();

    result.control
        .instrument
        .vectorscope
        .colorizeGamutErrors =
        settings.value(
            "Control/Instrument/Vectorscope/ColorizeGamutErrors",
            result.control
                .instrument
                .vectorscope
                .colorizeGamutErrors)
        .toBool();

    result.control
        .instrument
        .vectorscope
        .persistenceFrames =
        settings.value(
            "Control/Instrument/Vectorscope/PersistenceFrames",
            result.control
            .instrument
            .vectorscope
            .persistenceFrames)
        .toInt();

    result.control
        .instrument
        .vectorscope
        .glow =
        settings.value(
            "Control/Instrument/Vectorscope/Glow",
            result.control
            .instrument
            .vectorscope
            .glow)
        .toInt();

    result.control
        .processing
        .noiseFilter
        .enabled =
        settings.value(
            "Control/Processing/NoiseFilter/Enabled",
            result.control
            .processing
            .noiseFilter
            .enabled)
        .toBool();

    result.control
        .processing
        .noiseFilter
        .strength =
        settings.value(
            "Control/Processing/NoiseFilter/Strength",
            result.control
            .processing
            .noiseFilter
            .strength)
        .toInt();

    result.control
        .processing
        .noiseFilter
        .temporalStrength =
        settings.value(
            "Control/Processing/NoiseFilter/TemporalStrength",
            result.control
            .processing
            .noiseFilter
            .temporalStrength)
        .toInt();

    result.control
        .processing
        .lumaCompensation
        .enabled =
        settings.value(
            "Control/Processing/LumaCompensation/Enabled",
            result.control
            .processing
            .lumaCompensation
            .enabled)
        .toBool();

    result.control
        .processing
        .lumaCompensation
        .gainHundredthsDb =
        settings.value(
            "Control/Processing/LumaCompensation/GainHundredthsDb",
            result.control
            .processing
            .lumaCompensation
            .gainHundredthsDb)
        .toInt();

    result.control
        .pcmAudio
        .enabled =
        settings.value(
            "Control/PcmAudio/Enabled",
            result.control
            .pcmAudio
            .enabled)
        .toBool();

    result.control.pcmAudio.outputEnabled =
        settings.value(
            "Control/PcmAudio/OutputEnabled",
            result.control.pcmAudio.outputEnabled)
        .toBool();

    result.control.pcmAudio.outputBackend =
        std::clamp(
            settings.value(
                "Control/PcmAudio/OutputBackend",
                result.control.pcmAudio.outputBackend)
            .toInt(),
            0,
            1);

    result.control.pcmAudio.muteBottomTwoBits =
        settings.value(
            "Control/PcmAudio/MuteBottomTwoBits",
            result.control.pcmAudio.muteBottomTwoBits)
        .toBool();

    result.control.pcmAudio.outputDeviceId =
        settings.value(
            "Control/PcmAudio/OutputDeviceId",
            QString::fromStdString(
                result.control.pcmAudio.outputDeviceId))
        .toString()
        .toStdString();


    result.control.pcmAudio.asioDriverName =
        settings.value(
            "Control/PcmAudio/AsioDriverName",
            QString::fromStdString(
                result.control.pcmAudio.asioDriverName))
        .toString()
        .toStdString();

    result.control.pcmAudio.deEmphasisMode =
        std::clamp(
            settings.value(
                "Control/PcmAudio/DeEmphasisMode",
                result.control.pcmAudio.deEmphasisMode)
            .toInt(),
            0,
            2);

    {
        const int savedBufferMs =
            settings.value(
                "Control/PcmAudio/OutputBufferMs",
                result.control.pcmAudio.outputBufferMs)
            .toInt();
        constexpr int allowed[] = {0, 4, 8, 16, 32, 64, 128};
        int best = allowed[0];
        int bestDistance = std::abs(savedBufferMs - best);
        for (const int value : allowed)
        {
            const int distance = std::abs(savedBufferMs - value);
            if (distance < bestDistance)
            {
                best = value;
                bestDistance = distance;
            }
        }
        result.control.pcmAudio.outputBufferMs = best;
    }

    result.control.pcmAudio.audioScopeEnabled =
        settings.value(
            "Control/PcmAudio/AudioScopeEnabled",
            result.control.pcmAudio.audioScopeEnabled)
        .toBool();

    result.control.pcmAudio.audioScopeColorized =
        settings.value(
            "Control/PcmAudio/AudioScopeColorized",
            result.control.pcmAudio.audioScopeColorized)
        .toBool();

    result.control
        .videoOut
        .enabled =
        settings.value(
            "Control/VideoOut/Enabled",
            result.control
            .videoOut
            .enabled)
        .toBool();

    result.control
        .videoOut
        .width =
        settings.value(
            "Control/VideoOut/Width",
            result.control
            .videoOut
            .width)
        .toInt();

    result.control
        .videoOut
        .height =
        settings.value(
            "Control/VideoOut/Height",
            result.control
            .videoOut
            .height)
        .toInt();

    result.control
        .videoOut
        .aspectRatio =
        aspectRatioFromString(
            settings.value(
                "Control/VideoOut/AspectRatio",
                aspectRatioToString(
                    result.control
                    .videoOut
                    .aspectRatio))
            .toString(),
            result.control
            .videoOut
            .aspectRatio);

    result.control
        .videoOut
        .underscan =
        settings.value(
            "Control/VideoOut/Underscan",
            result.control
            .videoOut
            .underscan)
        .toDouble();

    result.control
        .videoOut
        .deinterlace =
        settings.value(
            "Control/VideoOut/Deinterlace",
            result.control
            .videoOut
            .deinterlace)
        .toBool();

    result.local.spout.videoEnabled =
        settings.value(
            "Local/Spout/Video/Enabled",
            result.local.spout.videoEnabled)
        .toBool();

    result.local.spout.waveformEnabled =
        settings.value(
            "Local/Spout/Waveform/Enabled",
            result.local.spout.waveformEnabled)
        .toBool();

    result.local.spout.vectorscopeEnabled =
        settings.value(
            "Local/Spout/Vectorscope/Enabled",
            result.local.spout.vectorscopeEnabled)
        .toBool();

    result.local.window.x =
        settings.value(
            "Local/Window/X",
            result.local.window.x)
        .toInt();

    result.local.window.y =
        settings.value(
            "Local/Window/Y",
            result.local.window.y)
        .toInt();

    result.local.window.width =
        settings.value(
            "Local/Window/Width",
            result.local.window.width)
        .toInt();

    result.local.window.height =
        settings.value(
            "Local/Window/Height",
            result.local.window.height)
        .toInt();

    result.local.window.maximized =
        settings.value(
            "Local/Window/Maximized",
            result.local.window.maximized)
        .toBool();

    result.local.workspace.view =
        workspaceViewFromString(
            settings.value(
                "Local/Workspace/View",
                workspaceViewToString(
                    result.local.workspace.view))
            .toString(),
            result.local.workspace.view);

    result.local.floaties.performance.x =
        settings.value(
            "Local/Floaties/Performance/X",
            result.local.floaties.performance.x)
        .toInt();

    result.local.floaties.performance.y =
        settings.value(
            "Local/Floaties/Performance/Y",
            result.local.floaties.performance.y)
        .toInt();

    result.local.floaties.performance.positionValid =
        settings.value(
            "Local/Floaties/Performance/PositionValid",
            result.local.floaties.performance.positionValid)
        .toBool();

    result.local.floaties.performanceVisible =
        settings.value(
            "Local/Floaties/Performance/Visible",
            result.local.floaties.performanceVisible)
        .toBool();

    // Waveform Video Out is diagnostic and must start disabled.
    // Ignore stale Visible=true values from older OpenScope.ini files; the
    // next settings save writes false back to the ini.
    result.local.floaties.waveformVideoVisible = false;

    result.local.floaties.settings.x =
        settings.value(
            "Local/Floaties/Settings/X",
            result.local.floaties.settings.x)
        .toInt();

    result.local.floaties.settings.y =
        settings.value(
            "Local/Floaties/Settings/Y",
            result.local.floaties.settings.y)
        .toInt();

    result.local.floaties.settings.positionValid =
        settings.value(
            "Local/Floaties/Settings/PositionValid",
            result.local.floaties.settings.positionValid)
        .toBool();

    result.local.floaties.settings.width =
        settings.value(
            "Local/Floaties/Settings/Width",
            result.local.floaties.settings.width)
        .toInt();

    result.local.floaties.settings.height =
        settings.value(
            "Local/Floaties/Settings/Height",
            result.local.floaties.settings.height)
        .toInt();

    result.local.floaties.settings.sizeValid =
        settings.value(
            "Local/Floaties/Settings/SizeValid",
            result.local.floaties.settings.sizeValid)
        .toBool();

    return result;
}

void SettingsStorage::save(
    const OpenScopeSettings& settings) const
{
    QSettings storage(
        fileName_,
        QSettings::IniFormat);

    storage.setValue(
        "Local/Display/Gamma",
        settings.local
        .display
        .gamma);

    storage.setValue(
        "Local/Display/AspectRatio",
        aspectRatioToString(
            settings.local
            .display
            .aspectRatio));

    storage.setValue(
        "Local/Display/Deinterlace",
        settings.local
        .display
        .deinterlace);

    storage.setValue(
        "Local/Display/LineSelectorVisible",
        settings.local.display.lineSelectorVisible);

    storage.setValue(
        "Local/Display/SafetyArea90",
        settings.local.display.safetyArea90);

    storage.setValue(
        "Local/Display/TextSafetyArea80",
        settings.local.display.textSafetyArea80);

    storage.setValue(
        "Local/Display/PreventDisplaySleep",
        settings.local.display.preventDisplaySleep);

    storage.setValue(
        "Local/Display/FollowWssAspectRatio",
        settings.local.display.followWssAspectRatio);

    storage.setValue(
        "Control/Instrument/LineNumber",
        settings.control
        .instrument
        .lineNumber);

    storage.setValue(
        "Control/Instrument/Waveform/AntiAliasing",
        settings.control
            .instrument
            .waveform
            .antiAliasing);

    storage.setValue(
        "Control/Instrument/Waveform/ColorizeIllegalLuminance",
        settings.control
            .instrument
            .waveform
            .colorizeIllegalLuminance);

    storage.setValue(
        "Control/Instrument/Waveform/Zoom",
        settings.control
        .instrument
        .waveform
        .zoom);

    storage.setValue(
        "Control/Instrument/Waveform/ScrollPosition",
        settings.control.instrument.waveform.scrollPosition);

    storage.setValue(
        "Control/Instrument/Waveform/VintageLook",
        settings.control
        .instrument
        .waveform
        .vintageLook);

    storage.setValue(
        "Control/Instrument/Waveform/ChromaRenderIntensity",
        settings.control
        .instrument
        .waveform
        .chromaRenderIntensity);

    storage.setValue(
        "Control/Instrument/Waveform/PersistenceFrames",
        settings.control
        .instrument
        .waveform
        .persistenceFrames);

    storage.setValue(
        "Control/Instrument/Waveform/CoreIntensity",
        settings.control
        .instrument
        .waveform
        .coreIntensity);

    storage.setValue(
        "Control/Instrument/Waveform/CoreWidthTenths",
        settings.control
        .instrument
        .waveform
        .coreWidthTenths);


    storage.setValue(
        "Control/Instrument/Vectorscope/ShowHundredPercentTargets",
        settings.control
        .instrument
        .vectorscope
        .showHundredPercentTargets);

    storage.setValue(
        "Control/Instrument/Vectorscope/ColorizeGamutErrors",
        settings.control
            .instrument
            .vectorscope
            .colorizeGamutErrors);

    storage.setValue(
        "Control/Instrument/Vectorscope/PersistenceFrames",
        settings.control
        .instrument
        .vectorscope
        .persistenceFrames);

    storage.setValue(
        "Control/Instrument/Vectorscope/Glow",
        settings.control
        .instrument
        .vectorscope
        .glow);

    storage.setValue(
        "Control/Processing/NoiseFilter/Enabled",
        settings.control
        .processing
        .noiseFilter
        .enabled);

    storage.setValue(
        "Control/Processing/NoiseFilter/Strength",
        settings.control
        .processing
        .noiseFilter
        .strength);

    storage.setValue(
        "Control/Processing/NoiseFilter/TemporalStrength",
        settings.control
        .processing
        .noiseFilter
        .temporalStrength);

    storage.setValue(
        "Control/Processing/LumaCompensation/Enabled",
        settings.control
        .processing
        .lumaCompensation
        .enabled);

    storage.setValue(
        "Control/Processing/LumaCompensation/GainHundredthsDb",
        settings.control
        .processing
        .lumaCompensation
        .gainHundredthsDb);

    storage.setValue(
        "Control/PcmAudio/Enabled",
        settings.control.pcmAudio.enabled);

    storage.setValue(
        "Control/PcmAudio/OutputEnabled",
        settings.control.pcmAudio.outputEnabled);


    storage.setValue(
        "Control/PcmAudio/OutputBackend",
        settings.control.pcmAudio.outputBackend);

    storage.setValue(
        "Control/PcmAudio/MuteBottomTwoBits",
        settings.control.pcmAudio.muteBottomTwoBits);

    storage.setValue(
        "Control/PcmAudio/OutputDeviceId",
        QString::fromStdString(
            settings.control.pcmAudio.outputDeviceId));


    storage.setValue(
        "Control/PcmAudio/AsioDriverName",
        QString::fromStdString(
            settings.control.pcmAudio.asioDriverName));

    storage.setValue(
        "Control/PcmAudio/DeEmphasisMode",
        settings.control.pcmAudio.deEmphasisMode);

    storage.setValue(
        "Control/PcmAudio/OutputBufferMs",
        settings.control.pcmAudio.outputBufferMs);

    storage.setValue(
        "Control/PcmAudio/AudioScopeEnabled",
        settings.control.pcmAudio.audioScopeEnabled);

    storage.setValue(
        "Control/PcmAudio/AudioScopeColorized",
        settings.control.pcmAudio.audioScopeColorized);

    storage.setValue(
        "Control/VideoOut/Enabled",
        settings.control
        .videoOut
        .enabled);

    storage.setValue(
        "Control/VideoOut/Width",
        settings.control
        .videoOut
        .width);

    storage.setValue(
        "Control/VideoOut/Height",
        settings.control
        .videoOut
        .height);

    storage.setValue(
        "Control/VideoOut/AspectRatio",
        aspectRatioToString(
            settings.control
            .videoOut
            .aspectRatio));

    storage.setValue(
        "Control/VideoOut/Underscan",
        settings.control
        .videoOut
        .underscan);

    storage.setValue(
        "Control/VideoOut/Deinterlace",
        settings.control
        .videoOut
        .deinterlace);

    storage.setValue(
        "Local/Spout/Video/Enabled",
        settings.local.spout.videoEnabled);

    storage.setValue(
        "Local/Spout/Waveform/Enabled",
        settings.local.spout.waveformEnabled);

    storage.setValue(
        "Local/Spout/Vectorscope/Enabled",
        settings.local.spout.vectorscopeEnabled);

    storage.setValue(
        "Local/Window/X",
        settings.local.window.x);

    storage.setValue(
        "Local/Window/Y",
        settings.local.window.y);

    storage.setValue(
        "Local/Window/Width",
        settings.local.window.width);

    storage.setValue(
        "Local/Window/Height",
        settings.local.window.height);

    storage.setValue(
        "Local/Window/Maximized",
        settings.local.window.maximized);

    storage.setValue(
        "Local/Workspace/View",
        workspaceViewToString(
            settings.local.workspace.view));

    storage.setValue(
        "Local/Floaties/Performance/X",
        settings.local.floaties.performance.x);

    storage.setValue(
        "Local/Floaties/Performance/Y",
        settings.local.floaties.performance.y);

    storage.setValue(
        "Local/Floaties/Performance/PositionValid",
        settings.local.floaties.performance.positionValid);

    storage.setValue(
        "Local/Floaties/Performance/Visible",
        settings.local.floaties.performanceVisible);

    storage.setValue(
        "Local/Floaties/WaveformVideo/Visible",
        settings.local.floaties.waveformVideoVisible);

    storage.setValue(
        "Local/Floaties/Settings/X",
        settings.local.floaties.settings.x);

    storage.setValue(
        "Local/Floaties/Settings/Y",
        settings.local.floaties.settings.y);

    storage.setValue(
        "Local/Floaties/Settings/PositionValid",
        settings.local.floaties.settings.positionValid);

    storage.setValue(
        "Local/Floaties/Settings/Width",
        settings.local.floaties.settings.width);

    storage.setValue(
        "Local/Floaties/Settings/Height",
        settings.local.floaties.settings.height);

    storage.setValue(
        "Local/Floaties/Settings/SizeValid",
        settings.local.floaties.settings.sizeValid);

    storage.sync();
}